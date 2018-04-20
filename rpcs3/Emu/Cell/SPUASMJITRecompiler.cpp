#include "stdafx.h"
#include "Emu/Memory/Memory.h"
#include "Emu/System.h"
#include "Emu/IdManager.h"

#include "SPUDisAsm.h"
#include "SPUThread.h"
#include "SPUInterpreter.h"
#include "Utilities/sysinfo.h"

#include <cmath>
#include <mutex>

#include "SPUASMJITRecompiler.h"

#define SPU_OFF_128(x, ...) asmjit::x86::oword_ptr(*cpu, offset32(&SPUThread::x, ##__VA_ARGS__))
#define SPU_OFF_64(x, ...) asmjit::x86::qword_ptr(*cpu, offset32(&SPUThread::x, ##__VA_ARGS__))
#define SPU_OFF_32(x, ...) asmjit::x86::dword_ptr(*cpu, offset32(&SPUThread::x, ##__VA_ARGS__))
#define SPU_OFF_16(x, ...) asmjit::x86::word_ptr(*cpu, offset32(&SPUThread::x, ##__VA_ARGS__))
#define SPU_OFF_8(x, ...) asmjit::x86::byte_ptr(*cpu, offset32(&SPUThread::x, ##__VA_ARGS__))

extern const spu_decoder<spu_interpreter_fast> g_spu_interpreter_fast; // TODO: avoid
const spu_decoder<spu_recompiler> s_spu_decoder;

extern u64 get_timebased_time();

std::unique_ptr<spu_recompiler_base> spu_recompiler_base::make_asmjit_recompiler(SPUThread& spu)
{
	return std::make_unique<spu_recompiler>(spu);
}

spu_runtime::spu_runtime()
{
	if (g_cfg.core.spu_debug)
	{
		fs::file log(Emu.GetCachePath() + "SPUJIT.log", fs::rewrite);
		log.write(fmt::format("SPU JIT Log...\n\nTitle: %s\nTitle ID: %s\n\n", Emu.GetTitle().c_str(), Emu.GetTitleID().c_str()));
	}

	LOG_SUCCESS(SPU, "SPU Recompiler Runtime (ASMJIT) initialized...");

	// Initialize lookup table
	for (auto& v : m_dispatcher)
	{
		v.raw() = &spu_recompiler_base::dispatch;
	}

	// Initialize "empty" block
	m_map[std::vector<u32>()] = &spu_recompiler_base::dispatch;
}

spu_recompiler::spu_recompiler(SPUThread& spu)
	: spu_recompiler_base(spu)
	, m_rt(std::make_shared<asmjit::JitRuntime>())
{
}

spu_function_t spu_recompiler::compile(const std::vector<u32>& func)
{
	// Initialize if necessary
	if (!m_spurt)
	{
		m_spurt = fxm::get_always<spu_runtime>();
	}

	using namespace asmjit;

	SPUDisAsm dis_asm(CPUDisAsm_InterpreterMode);
	dis_asm.offset = reinterpret_cast<const u8*>(func.data() + 1) - func[0];

	StringLogger logger;
	logger.addOptions(Logger::kOptionBinaryForm);

	std::string log;

	if (g_cfg.core.spu_debug)
	{
		fmt::append(log, "========== SPU BLOCK 0x%05x (size %u) ==========\n\n", func[0], func.size() - 1);
	}

	asmjit::CodeHolder code;
	code.init(m_rt->getCodeInfo());

	X86Assembler compiler(&code);
	this->c = &compiler;

	if (g_cfg.core.spu_debug)
	{
		// Set logger
		code.setLogger(&logger);
	}

	// Initialize variables
#ifdef _WIN32
	this->cpu = &x86::rcx;
	this->ls = &x86::rdx;
#else
	this->cpu = &x86::rdi;
	this->ls = &x86::rsi;
#endif

	this->addr = &x86::eax;
#ifdef _WIN32
	this->qw0 = &x86::r8;
	this->qw1 = &x86::r9;
#else
	this->qw0 = &x86::rdx;
	this->qw1 = &x86::rcx;
#endif

	const std::array<const X86Xmm*, 6> vec_vars
	{
		&x86::xmm0,
		&x86::xmm1,
		&x86::xmm2,
		&x86::xmm3,
		&x86::xmm4,
		&x86::xmm5,
	};

	for (u32 i = 0; i < vec_vars.size(); i++)
	{
		vec[i] = vec_vars[i];
	}

	Label label_stop = c->newLabel();
	Label label_diff = c->newLabel();
	Label label_code = c->newLabel();
	std::vector<u32> words;
	u32 words_align = 8;

	// Start compilation
	m_pos = func[0];

	// Set PC and check status
	c->mov(SPU_OFF_32(pc), m_pos);
	c->cmp(SPU_OFF_32(state), 0);
	c->jnz(label_stop);

	if (utils::has_avx())
	{
		// How to check dirty AVX state
		//c->pxor(x86::xmm0, x86::xmm0);
		//c->vptest(x86::ymm0, x86::ymm0);
		//c->jnz(label_stop);
	}

	// Check code
	if (false)
	{
		// Disable check (not available)
	}
	else if (func.size() - 1 == 1)
	{
		c->cmp(x86::dword_ptr(*ls, m_pos), func[1]);
		c->jnz(label_diff);
	}
	else if (func.size() - 1 == 2)
	{
		c->mov(*qw1, static_cast<u64>(func[2]) << 32 | func[1]);
		c->cmp(*qw1, x86::qword_ptr(*ls, m_pos));
		c->jnz(label_diff);
	}
	else if (utils::has_512() && false)
	{
		// AVX-512 optimized check using 512-bit registers (disabled)
		words_align = 64;

		const u32 starta = m_pos & -64;
		const u32 end = m_pos + (func.size() - 1) * 4;
		const u32 enda = ::align(end, 64);
		const u32 sizea = (enda - starta) / 64;
		verify(HERE), sizea;

		// Load masks
		if (m_pos != starta || sizea == 1)
		{
			Label label = c->newLabel();
			c->kmovw(x86::k1, x86::word_ptr(label));
			const u16 mask = (0xffff << (m_pos - starta) / 4) & (0xffff >> (sizea == 1 ? enda - end : 0) / 4);

			consts.emplace_back([=]
			{
				c->bind(label);
				c->dw(mask);
			});
		}

		if (sizea > 1 && end != enda && end + 32 != enda)
		{
			Label label = c->newLabel();
			c->kmovw(x86::k2, x86::word_ptr(label));
			const u16 mask = 0xffff >> (enda - end) / 4;

			consts.emplace_back([=]
			{
				c->bind(label);
				c->dw(mask);
			});
		}

		// Initialize pointers
		c->lea(x86::rax, x86::qword_ptr(label_code));
		c->lea(*qw1, x86::qword_ptr(*ls, starta));

		for (u32 j = starta; j < enda; j += 64)
		{
			// Small offset for disp8*N
			const u32 off = (j - starta) % 8192;

			if (j != starta && off == 0)
			{
				// Almost unexpected: update pointers
				c->lea(x86::rax, x86::qword_ptr(label_code, j));
				c->lea(*qw1, x86::qword_ptr(*ls, j));
			}

			if (j < m_pos || j + 64 > end)
			{
				c->setExtraReg(j < m_pos || sizea == 1 ? x86::k1 : x86::k2);
				c->z().vmovdqa32(x86::zmm0, x86::zword_ptr(*qw1, off));
			}
			else
			{
				c->vmovdqa32(x86::zmm0, x86::zword_ptr(*qw1, off));
			}

			if (j == starta)
			{
				c->vpcmpud(x86::k1, x86::zmm0, x86::zword_ptr(x86::rax, off), 4);
			}
			else
			{
				c->vpcmpud(x86::k3, x86::zmm0, x86::zword_ptr(x86::rax, off), 4);
				c->korw(x86::k1, x86::k3, x86::k1);
			}
		}

		c->ktestw(x86::k1, x86::k1);
		c->jnz(label_diff);

		for (u32 i = starta; i < enda; i += 4)
		{
			words.push_back(i >= m_pos && i < end ? func[(i - m_pos) / 4 + 1] : 0);
		}
	}
	else if (utils::has_512())
	{
		// AVX-512 optimized check using 256-bit registers
		words_align = 32;

		const u32 starta = m_pos & -32;
		const u32 end = m_pos + (func.size() - 1) * 4;
		const u32 enda = ::align(end, 32);
		const u32 sizea = (enda - starta) / 32;
		verify(HERE), sizea;

		if (sizea == 1)
		{
			if (starta == m_pos && enda == end)
			{
				c->vmovdqa(x86::ymm0, x86::yword_ptr(*ls, starta));
			}
			else
			{
				c->vpxor(x86::ymm0, x86::ymm0, x86::ymm0);
				c->vpblendd(x86::ymm0, x86::ymm0, x86::yword_ptr(*ls, starta), (0xff << (m_pos - starta) / 4) & (0xff >> (enda - end) / 4));
			}

			c->vpxor(x86::ymm0, x86::ymm0, x86::yword_ptr(label_code));
			c->vptest(x86::ymm0, x86::ymm0);
			c->jnz(label_diff);

			for (u32 i = starta; i < enda; i += 4)
			{
				words.push_back(i >= m_pos && i < end ? func[(i - m_pos) / 4 + 1] : 0);
			}
		}
		else if (sizea == 2 && (end - m_pos) <= 32)
		{
			c->vpxor(x86::ymm0, x86::ymm0, x86::ymm0);
			c->vpblendd(x86::ymm0, x86::ymm0, x86::yword_ptr(*ls, starta), 0xff & (0xff << (m_pos - starta) / 4));
			c->vpblendd(x86::ymm0, x86::ymm0, x86::yword_ptr(*ls, starta + 32), 0xff & (0xff >> (enda - end) / 4));
			c->vpxor(x86::ymm0, x86::ymm0, x86::yword_ptr(label_code));
			c->vptest(x86::ymm0, x86::ymm0);
			c->jnz(label_diff);

			for (u32 i = starta; i < starta + 32; i += 4)
			{
				words.push_back(i >= m_pos ? func[(i - m_pos) / 4 + 1] : i + 32 < end ? func[(i + 32 - m_pos) / 4 + 1] : 0);
			}
		}
		else
		{
			if (starta < m_pos || enda > end)
			{
				c->vpxor(x86::xmm2, x86::xmm2, x86::xmm2);
			}

			// Initialize pointers
			c->lea(x86::rax, x86::qword_ptr(label_code));
			c->lea(*qw1, x86::qword_ptr(*ls, starta));

			for (u32 j = starta; j < enda; j += 32)
			{
				// Small offset for disp8*N
				const u32 off = (j - starta) % 4096;

				if (j != starta && off == 0)
				{
					// Almost unexpected: update pointers
					c->lea(x86::rax, x86::qword_ptr(label_code, j - starta));
					c->lea(*qw1, x86::qword_ptr(*ls, j));
				}

				// Load aligned code block from LS, mask if necessary (at the end or the beginning)
				if (j < m_pos)
				{
					c->vpblendd(x86::ymm1, x86::ymm2, x86::yword_ptr(*qw1, off), 0xff & (0xff << (m_pos - starta) / 4));
				}
				else if (j + 32 > end)
				{
					c->vpblendd(x86::ymm1, x86::ymm2, x86::yword_ptr(*qw1, off), 0xff & (0xff >> (enda - end) / 4));
				}
				else
				{
					c->vmovdqa32(x86::ymm1, x86::yword_ptr(*qw1, off));
				}

				// Perform bitwise comparison and accumulate
				if (j == starta)
				{
					c->vpxor(x86::ymm0, x86::ymm1, x86::yword_ptr(x86::rax, off));
				}
				else
				{
					c->vpternlogd(x86::ymm0, x86::ymm1, x86::yword_ptr(x86::rax, off), 0xf6 /* orAxorBC */);
				}
			}

			c->vptest(x86::ymm0, x86::ymm0);
			c->jnz(label_diff);

			for (u32 i = starta; i < enda; i += 4)
			{
				words.push_back(i >= m_pos && i < end ? func[(i - m_pos) / 4 + 1] : 0);
			}
		}
	}
	else if (utils::has_avx())
	{
		// Mainstream AVX
		words_align = 32;

		const u32 starta = m_pos & -32;
		const u32 end = m_pos + (func.size() - 1) * 4;
		const u32 enda = ::align(end, 32);
		const u32 sizea = (enda - starta) / 32;
		verify(HERE), sizea;

		if (sizea == 1)
		{
			if (starta == m_pos && enda == end)
			{
				c->vmovaps(x86::ymm0, x86::yword_ptr(*ls, starta));
			}
			else
			{
				c->vxorps(x86::ymm0, x86::ymm0, x86::ymm0);
				c->vblendps(x86::ymm0, x86::ymm0, x86::yword_ptr(*ls, starta), (0xff << (m_pos - starta) / 4) & (0xff >> (enda - end) / 4));
			}

			c->vxorps(x86::ymm0, x86::ymm0, x86::yword_ptr(label_code));
			c->vptest(x86::ymm0, x86::ymm0);
			c->jnz(label_diff);

			for (u32 i = starta; i < enda; i += 4)
			{
				words.push_back(i >= m_pos && i < end ? func[(i - m_pos) / 4 + 1] : 0);
			}
		}
		else if (sizea == 2 && (end - m_pos) <= 32)
		{
			c->vxorps(x86::ymm0, x86::ymm0, x86::ymm0);
			c->vblendps(x86::ymm0, x86::ymm0, x86::yword_ptr(*ls, starta), 0xff & (0xff << (m_pos - starta) / 4));
			c->vblendps(x86::ymm0, x86::ymm0, x86::yword_ptr(*ls, starta + 32), 0xff & (0xff >> (enda - end) / 4));
			c->vxorps(x86::ymm0, x86::ymm0, x86::yword_ptr(label_code));
			c->vptest(x86::ymm0, x86::ymm0);
			c->jnz(label_diff);

			for (u32 i = starta; i < starta + 32; i += 4)
			{
				words.push_back(i >= m_pos ? func[(i - m_pos) / 4 + 1] : i + 32 < end ? func[(i + 32 - m_pos) / 4 + 1] : 0);
			}
		}
		else
		{
			if (starta < m_pos || enda > end)
			{
				c->vxorps(x86::xmm2, x86::xmm2, x86::xmm2);
			}

			// Initialize pointers
			c->add(*ls, starta);
			c->lea(x86::rax, x86::qword_ptr(label_code));
			u32 ls_off = starta;

			for (u32 j = starta; j < enda; j += 32)
			{
				// Small offset
				const u32 off = (j - starta) % 128;

				// Interleave two threads
				const auto& reg0 = off % 64 ? x86::ymm3 : x86::ymm0;
				const auto& reg1 = off % 64 ? x86::ymm4 : x86::ymm1;

				if (j != starta && off == 0)
				{
					ls_off += 128;
					c->sub(*ls, -128);
					c->sub(x86::rax, -128);
				}

				// Load aligned code block from LS, mask if necessary (at the end or the beginning)
				if (j < m_pos)
				{
					c->vblendps(reg1, x86::ymm2, x86::yword_ptr(*ls, off), 0xff & (0xff << (m_pos - starta) / 4));
				}
				else if (j + 32 > end)
				{
					c->vblendps(reg1, x86::ymm2, x86::yword_ptr(*ls, off), 0xff & (0xff >> (enda - end) / 4));
				}
				else
				{
					c->vmovaps(reg1, x86::yword_ptr(*ls, off));
				}

				// Perform bitwise comparison and accumulate
				if (j == starta || j == starta + 32)
				{
					c->vxorps(reg0, reg1, x86::yword_ptr(x86::rax, off));
				}
				else
				{
					c->vxorps(reg1, reg1, x86::yword_ptr(x86::rax, off));
					c->vorps(reg0, reg1, reg0);
				}
			}

			c->sub(*ls, ls_off);
			c->vorps(x86::ymm0, x86::ymm3, x86::ymm0);
			c->vptest(x86::ymm0, x86::ymm0);
			c->jnz(label_diff);

			for (u32 i = starta; i < enda; i += 4)
			{
				words.push_back(i >= m_pos && i < end ? func[(i - m_pos) / 4 + 1] : 0);
			}
		}
	}
	else if (true)
	{
		// Compatible SSE2
		words_align = 16;

		const u32 starta = m_pos & -16;
		const u32 end = m_pos + (func.size() - 1) * 4;
		const u32 enda = ::align(end, 16);
		const u32 sizea = (enda - starta) / 16;
		verify(HERE), sizea;

		// Initialize pointers
		c->add(*ls, starta);
		c->lea(x86::rax, x86::qword_ptr(label_code));
		u32 ls_off = starta;

		for (u32 j = starta; j < enda; j += 16)
		{
			// Small offset
			const u32 off = (j - starta) % 128;

			// Interleave two threads
			const auto& reg0 = off % 32 ? x86::xmm3 : x86::xmm0;
			const auto& reg1 = off % 32 ? x86::xmm4 : x86::xmm1;
			const auto& dest = j == starta || j == starta + 16 ? reg0 : reg1;

			if (j != starta && off == 0)
			{
				ls_off += 128;
				c->sub(*ls, -128);
				c->sub(x86::rax, -128);
			}

			// Load aligned code block from LS
			if (j < m_pos)
			{
				static constexpr u8 s_masks[4]{0b11100100, 0b11100101, 0b11101010, 0b11111111};
				c->pshufd(dest, x86::dqword_ptr(*ls, off), s_masks[(m_pos - starta) / 4]);
			}
			else if (j + 16 > end)
			{
				static constexpr u8 s_masks[4]{0b11100100, 0b10100100, 0b01010100, 0b00000000};
				c->pshufd(dest, x86::dqword_ptr(*ls, off), s_masks[(enda - end) / 4]);
			}
			else
			{
				c->movaps(dest, x86::dqword_ptr(*ls, off));
			}

			// Perform bitwise comparison and accumulate
			c->xorps(dest, x86::dqword_ptr(x86::rax, off));

			if (j != starta && j != starta + 16)
			{
				c->orps(reg0, dest);
			}
		}

		for (u32 i = starta; i < enda; i += 4)
		{
			// Fill alignment holes with first or last elements
			words.push_back(func[(i < m_pos ? 0 : i >= end ? end - 4 - m_pos : i - m_pos) / 4 + 1]);
		}

		if (sizea != 1)
		{
			c->orps(x86::xmm0, x86::xmm3);
		}

		c->sub(*ls, ls_off);

		if (utils::has_sse41())
		{
			c->ptest(x86::xmm0, x86::xmm0);
			c->jnz(label_diff);
		}
		else
		{
			c->packssdw(x86::xmm0, x86::xmm0);
			c->movq(x86::rax, x86::xmm0);
			c->test(x86::rax, x86::rax);
			c->jne(label_diff);
		}
	}
	else
	{
		// Legacy (slow, disabled)
		save_rcx();
		c->mov(x86::r9, x86::rdi);
		c->mov(x86::r10, x86::rsi);
		c->lea(x86::rsi, x86::qword_ptr(*ls, m_pos));
		c->lea(x86::rdi, x86::qword_ptr(label_code));
		c->mov(x86::ecx, (func.size() - 1) / 2);
		if ((func.size() - 1) % 2)
			c->cmpsd();
		c->repe().cmpsq();
		load_rcx();
		c->mov(x86::rdi, x86::r9);
		c->mov(x86::rsi, x86::r10);
		c->jnz(label_diff);

		for (u32 i = 1; i < func.size(); i++)
		{
			words.push_back(func[i]);
		}
	}

	if (utils::has_avx())
	{
		c->vzeroupper();
	}

	c->inc(SPU_OFF_64(block_counter));

	for (u32 i = 1; i < func.size(); i++)
	{
		if (g_cfg.core.spu_debug)
		{
			// Disasm
			dis_asm.dump_pc = m_pos;
			dis_asm.disasm(m_pos);
			compiler.comment(dis_asm.last_opcode.c_str());
			log += dis_asm.last_opcode;
			log += '\n';
		}

		// Get opcode
		const u32 op = se_storage<u32>::swap(func[i]);

		// Execute recompiler function
		(this->*s_spu_decoder.decode(op))({op});

		// Collect allocated xmm vars
		for (u32 i = 0; i < vec_vars.size(); i++)
		{
			vec[i] = vec_vars[i];
		}

		// Check if block was terminated
		if (m_pos == -1)
		{
			break;
		}

		// Set next position
		m_pos += 4;
	}

	if (g_cfg.core.spu_debug)
	{
		log += '\n';
	}

	// Make fallthrough if necessary
	if (m_pos != -1)
	{
		branch_fixed(spu_branch_target(m_pos));
	}

	// Simply return
	c->align(kAlignCode, 16);
	c->bind(label_stop);
	c->ret();

	// Dispatch
	c->align(kAlignCode, 16);
	c->bind(label_diff);
	c->inc(SPU_OFF_64(block_failure));
	c->jmp(asmjit::imm_ptr(&spu_recompiler_base::dispatch));

	for (auto&& work : decltype(after)(std::move(after)))
	{
		work();
	}

	c->align(kAlignData, words_align);
	c->bind(label_code);
	for (u32 d : words)
		c->dd(d);

	for (auto&& work : decltype(consts)(std::move(consts)))
	{
		work();
	}

	xmm_consts.clear();

	// Compile and get function address
	spu_function_t fn;

	if (m_rt->add(&fn, &code))
	{
		LOG_FATAL(SPU, "Failed to build a function");
	}

	if (g_cfg.core.spu_debug)
	{
		// Add ASMJIT logs
		fmt::append(log, "{%s} Address: %p\n\n", m_spu.get_name(), fn);
		log += logger.getString();
		log += "\n\n\n";

		// Append log file
		fs::file(Emu.GetCachePath() + "SPUJIT.log", fs::write + fs::append).write(log);
	}

	return fn;
}

spu_recompiler::XmmLink spu_recompiler::XmmAlloc() // get empty xmm register
{
	for (auto& v : vec)
	{
		if (v) return{ v };
	}

	fmt::throw_exception("Out of Xmm Vars" HERE);
}

spu_recompiler::XmmLink spu_recompiler::XmmGet(s8 reg, XmmType type) // get xmm register with specific SPU reg
{
	XmmLink result = XmmAlloc();

	switch (type)
	{
	case XmmType::Int: c->movdqa(result, SPU_OFF_128(gpr, reg)); break;
	case XmmType::Float: c->movaps(result, SPU_OFF_128(gpr, reg)); break;
	case XmmType::Double: c->movapd(result, SPU_OFF_128(gpr, reg)); break;
	default: fmt::throw_exception("Invalid XmmType" HERE);
	}

	return result;
}

inline asmjit::X86Mem spu_recompiler::XmmConst(v128 data)
{
	// Find existing const
	auto& xmm_label = xmm_consts[std::make_pair(data._u64[0], data._u64[1])];

	if (!xmm_label.isValid())
	{
		xmm_label = c->newLabel();

		consts.emplace_back([=]
		{
			c->align(asmjit::kAlignData, 16);
			c->bind(xmm_label);
			c->dq(data._u64[0]);
			c->dq(data._u64[1]);
		});
	}

	return asmjit::x86::oword_ptr(xmm_label);
}

inline asmjit::X86Mem spu_recompiler::XmmConst(__m128 data)
{
	return XmmConst(v128::fromF(data));
}

inline asmjit::X86Mem spu_recompiler::XmmConst(__m128i data)
{
	return XmmConst(v128::fromV(data));
}

void spu_recompiler::branch_fixed(u32 target)
{
	using namespace asmjit;

	// Set patch address as a third argument and fallback to it
	Label patch_point = c->newLabel();
	c->lea(*qw0, x86::qword_ptr(patch_point));
	c->mov(SPU_OFF_32(pc), target);
	c->align(kAlignCode, 16);
	c->bind(patch_point);

	const auto result = m_spu.jit_map.emplace(block(m_spu, target), nullptr);

	if (result.second || !result.first->second)
	{
		if (result.first->first.size())
		{
			// Target block hasn't been compiled yet, record overwriting position
			c->mov(*ls, imm_ptr(&*result.first));
			c->jmp(imm_ptr(&spu_recompiler_base::branch));
		}
		else
		{
			// SPURS Workload entry point or similar thing
			c->mov(x86::r10, x86::qword_ptr(*cpu, offset32(&SPUThread::jit_dispatcher) + target * 2));
			c->xor_(qw0->r32(), qw0->r32());
			c->jmp(x86::r10);
		}
	}
	else
	{
		c->jmp(imm_ptr(result.first->second));
	}

	c->align(kAlignCode, 16);
}

void spu_recompiler::branch_indirect(spu_opcode_t op)
{
	using namespace asmjit;

	// Load indirect jump address
	c->mov(x86::r10, x86::qword_ptr(*cpu, addr->r64(), 1, offset32(&SPUThread::jit_dispatcher)));
	c->xor_(qw0->r32(), qw0->r32());

	if (op.d)
	{
		c->lock().btr(SPU_OFF_8(interrupts_enabled), 0);
	}
	else if (op.e)
	{
		c->lock().bts(SPU_OFF_8(interrupts_enabled), 0);
		c->mov(x86::r9d, SPU_OFF_32(ch_event_stat));
		c->and_(x86::r9d, SPU_OFF_32(ch_event_mask));
		c->and_(x86::r9d, SPU_EVENT_INTR_TEST);
		c->cmp(x86::r9d, 0);

		Label noInterrupt = c->newLabel();
		c->je(noInterrupt);
		c->lock().btr(SPU_OFF_8(interrupts_enabled), 0);
		c->mov(SPU_OFF_32(srr0), *addr);
		branch_fixed(0);
		c->align(kAlignCode, 16);
		c->bind(noInterrupt);
	}

	c->mov(SPU_OFF_32(pc), *addr);
	c->jmp(x86::r10);
}

asmjit::Label spu_recompiler::halt(u32 pos)
{
	auto gate = [](SPUThread* _spu)
	{
		_spu->halt();
	};

	asmjit::Label label = c->newLabel();

	after.emplace_back([=]
	{
		c->align(asmjit::kAlignCode, 16);
		c->bind(label);
		c->mov(SPU_OFF_32(pc), pos);
		c->jmp(asmjit::imm_ptr<void(*)(SPUThread*)>(gate));
	});

	return label;
}

void spu_recompiler::fall(spu_opcode_t op)
{
	auto gate = [](SPUThread* _spu, u32 opcode, spu_inter_func_t _func, spu_function_t _ret)
	{
		if (LIKELY(_func(*_spu, {opcode})))
		{
			// Restore arguments and return to the next instruction
			_ret(*_spu, _spu->_ptr<u8>(0), nullptr);
		}
	};

	asmjit::Label next = c->newLabel();
	c->mov(SPU_OFF_32(pc), m_pos);
	c->mov(*ls, op.opcode);
	c->mov(*qw0, asmjit::imm_ptr(asmjit::Internal::ptr_cast<void*>(g_spu_interpreter_fast.decode(op.opcode))));
	c->lea(*qw1, asmjit::x86::qword_ptr(next));
	c->jmp(asmjit::imm_ptr<void(*)(SPUThread*, u32, spu_inter_func_t, spu_function_t)>(gate));
	c->align(asmjit::kAlignCode, 16);
	c->bind(next);
}

void spu_recompiler::save_rcx()
{
#ifdef _WIN32
	c->mov(asmjit::x86::r11, *cpu);
	cpu = &asmjit::x86::r11;
#endif
}

void spu_recompiler::load_rcx()
{
#ifdef _WIN32
	cpu = &asmjit::x86::rcx;
	c->mov(*cpu, asmjit::x86::r11);
#endif
}

void spu_recompiler::UNK(spu_opcode_t op)
{
	auto gate = [](SPUThread* _spu, u32 op)
	{
		fmt::throw_exception("Unknown/Illegal instruction (0x%08x)" HERE, op);
	};

	c->mov(SPU_OFF_32(pc), m_pos);
	c->mov(*ls, op.opcode);
	c->jmp(asmjit::imm_ptr<void(*)(SPUThread*, u32)>(gate));
	m_pos = -1;
}

void spu_recompiler::STOP(spu_opcode_t op)
{
	auto gate = [](SPUThread* _spu, u32 code)
	{
		if (_spu->stop_and_signal(code))
		{
			_spu->pc += 4;
		}
	};

	c->mov(SPU_OFF_32(pc), m_pos);
	c->mov(*ls, op.opcode);
	c->jmp(asmjit::imm_ptr<void(*)(SPUThread*, u32)>(gate));
	m_pos = -1;
}

void spu_recompiler::LNOP(spu_opcode_t op)
{
}

void spu_recompiler::SYNC(spu_opcode_t op)
{
	// This instruction must be used following a store instruction that modifies the instruction stream.
	c->mfence();
}

void spu_recompiler::DSYNC(spu_opcode_t op)
{
	// This instruction forces all earlier load, store, and channel instructions to complete before proceeding.
	c->mfence();
}

void spu_recompiler::MFSPR(spu_opcode_t op)
{
	// Check SPUInterpreter for notes.
	const XmmLink& vr = XmmAlloc();
	c->pxor(vr, vr);
	c->movdqa(SPU_OFF_128(gpr, op.rt), vr);
}

void spu_recompiler::RDCH(spu_opcode_t op)
{
	auto gate = [](SPUThread* _spu, u32 ch, v128* out)
	{
		u32 value;

		if (_spu->get_ch_value(ch, value))
		{
			*out = v128::from32r(value);
			_spu->pc += 4;
		}
	};

	switch (op.ra)
	{
	case SPU_RdSRR0:
	{
		const XmmLink& vr = XmmAlloc();
		c->movd(vr, SPU_OFF_32(srr0));
		c->pslldq(vr, 12);
		c->movdqa(SPU_OFF_128(gpr, op.rt), vr);
		return;
	}
	case MFC_RdTagMask:
	{
		const XmmLink& vr = XmmAlloc();
		c->movd(vr, SPU_OFF_32(ch_tag_mask));
		c->pslldq(vr, 12);
		c->movdqa(SPU_OFF_128(gpr, op.rt), vr);
		return;
	}
	case SPU_RdEventMask:
	{
		const XmmLink& vr = XmmAlloc();
		c->movd(vr, SPU_OFF_32(ch_event_mask));
		c->pslldq(vr, 12);
		c->movdqa(SPU_OFF_128(gpr, op.rt), vr);
		return;
	}
	}

	c->mov(SPU_OFF_32(pc), m_pos);
	c->mov(*ls, op.ra);
	c->lea(*qw0, SPU_OFF_128(gpr, op.rt));
	c->jmp(asmjit::imm_ptr<void(*)(SPUThread*, u32, v128*)>(gate));
	m_pos = -1;
}

void spu_recompiler::RCHCNT(spu_opcode_t op)
{
	auto gate = [](SPUThread* _spu, u32 ch, v128* out)
	{
		*out = v128::from32r(_spu->get_ch_count(ch));
		_spu->pc += 4;
	};

	c->mov(SPU_OFF_32(pc), m_pos);
	c->mov(*ls, op.ra);
	c->lea(*qw0, SPU_OFF_128(gpr, op.rt));
	c->jmp(asmjit::imm_ptr<void(*)(SPUThread*, u32, v128*)>(gate));
	m_pos = -1;
}

void spu_recompiler::SF(spu_opcode_t op)
{
	// sub from
	const XmmLink& vb = XmmGet(op.rb, XmmType::Int);
	c->psubd(vb, SPU_OFF_128(gpr, op.ra));
	c->movdqa(SPU_OFF_128(gpr, op.rt), vb);
}

void spu_recompiler::OR(spu_opcode_t op)
{
	const XmmLink& vb = XmmGet(op.rb, XmmType::Int);
	c->por(vb, SPU_OFF_128(gpr, op.ra));
	c->movdqa(SPU_OFF_128(gpr, op.rt), vb);
}

void spu_recompiler::BG(spu_opcode_t op)
{
	// compare if-greater-than
	const XmmLink& va = XmmGet(op.ra, XmmType::Int);
	const XmmLink& vi = XmmAlloc();

	if (utils::has_512())
	{
		const XmmLink& vb = XmmGet(op.rb, XmmType::Int);
		c->vpsubd(vi, vb, va);
		c->vpternlogd(va, vb, vi, 0x4d /* B?nandAC:norAC */);
		c->psrld(va, 31);
		c->movdqa(SPU_OFF_128(gpr, op.rt), va);
		return;
	}

	c->movdqa(vi, XmmConst(_mm_set1_epi32(0x80000000)));
	c->pxor(va, vi);
	c->pxor(vi, SPU_OFF_128(gpr, op.rb));
	c->pcmpgtd(va, vi);
	c->paddd(va, XmmConst(_mm_set1_epi32(1)));
	c->movdqa(SPU_OFF_128(gpr, op.rt), va);
}

void spu_recompiler::SFH(spu_opcode_t op)
{
	// sub from (halfword)
	const XmmLink& vb = XmmGet(op.rb, XmmType::Int);
	c->psubw(vb, SPU_OFF_128(gpr, op.ra));
	c->movdqa(SPU_OFF_128(gpr, op.rt), vb);
}

void spu_recompiler::NOR(spu_opcode_t op)
{
	const XmmLink& va = XmmGet(op.ra, XmmType::Int);

	if (utils::has_512())
	{
		c->vpternlogd(va, va, SPU_OFF_128(gpr, op.rb), 0x11 /* norCB */);
		c->movdqa(SPU_OFF_128(gpr, op.rt), va);
		return;
	}

	c->por(va, SPU_OFF_128(gpr, op.rb));
	c->pxor(va, XmmConst(_mm_set1_epi32(0xffffffff)));
	c->movdqa(SPU_OFF_128(gpr, op.rt), va);
}

void spu_recompiler::ABSDB(spu_opcode_t op)
{
	const XmmLink& va = XmmGet(op.ra, XmmType::Int);
	const XmmLink& vb = XmmGet(op.rb, XmmType::Int);
	const XmmLink& vm = XmmAlloc();
	c->movdqa(vm, va);
	c->pmaxub(va, vb);
	c->pminub(vb, vm);
	c->psubb(va, vb);
	c->movdqa(SPU_OFF_128(gpr, op.rt), va);
}

void spu_recompiler::ROT(spu_opcode_t op)
{
	if (utils::has_512())
	{
		const XmmLink& va = XmmGet(op.ra, XmmType::Int);
		const XmmLink& vb = XmmGet(op.rb, XmmType::Int);
		const XmmLink& vt = XmmAlloc();
		c->vprolvd(vt, va, vb);
		c->movdqa(SPU_OFF_128(gpr, op.rt), vt);
		return;
	}

	if (utils::has_avx2())
	{
		const XmmLink& va = XmmGet(op.ra, XmmType::Int);
		const XmmLink& vb = XmmGet(op.rb, XmmType::Int);
		const XmmLink& vt = XmmAlloc();
		const XmmLink& v4 = XmmAlloc();
		c->movdqa(v4, XmmConst(_mm_set1_epi32(0x1f)));
		c->pand(vb, v4);
		c->vpsllvd(vt, va, vb);
		c->psubd(vb, XmmConst(_mm_set1_epi32(1)));
		c->pandn(vb, v4);
		c->vpsrlvd(va, va, vb);
		c->por(vt, va);
		c->movdqa(SPU_OFF_128(gpr, op.rt), vt);
		return;
	}

	if (utils::has_xop())
	{
		const XmmLink& va = XmmGet(op.ra, XmmType::Int);
		const XmmLink& vb = XmmGet(op.rb, XmmType::Int);
		const XmmLink& vt = XmmAlloc();
		c->vprotd(vt, va, vb);
		c->movdqa(SPU_OFF_128(gpr, op.rt), vt);
		return;
	}

	save_rcx();

	for (u32 i = 0; i < 4; i++) // unrolled loop
	{
		c->mov(qw0->r32(), SPU_OFF_32(gpr, op.ra, &v128::_u32, i));
		c->mov(asmjit::x86::ecx, SPU_OFF_32(gpr, op.rb, &v128::_u32, i));
		c->rol(qw0->r32(), asmjit::x86::cl);
		c->mov(SPU_OFF_32(gpr, op.rt, &v128::_u32, i), qw0->r32());
	}

	load_rcx();
}

void spu_recompiler::ROTM(spu_opcode_t op)
{
	if (utils::has_avx2())
	{
		const XmmLink& va = XmmGet(op.ra, XmmType::Int);
		const XmmLink& vb = XmmGet(op.rb, XmmType::Int);
		const XmmLink& vt = XmmAlloc();
		c->psubd(vb, XmmConst(_mm_set1_epi32(1)));
		c->pandn(vb, XmmConst(_mm_set1_epi32(0x3f)));
		c->vpsrlvd(vt, va, vb);
		c->movdqa(SPU_OFF_128(gpr, op.rt), vt);
		return;
	}

	if (utils::has_xop())
	{
		const XmmLink& va = XmmGet(op.ra, XmmType::Int);
		const XmmLink& vb = XmmGet(op.rb, XmmType::Int);
		const XmmLink& vt = XmmAlloc();
		c->psubd(vb, XmmConst(_mm_set1_epi32(1)));
		c->pandn(vb, XmmConst(_mm_set1_epi32(0x3f)));
		c->pxor(vt, vt);
		c->psubd(vt, vb);
		c->pcmpgtd(vb, XmmConst(_mm_set1_epi32(31)));
		c->vpshld(vt, va, vt);
		c->vpandn(vt, vb, vt);
		c->movdqa(SPU_OFF_128(gpr, op.rt), vt);
		return;
	}

	save_rcx();

	for (u32 i = 0; i < 4; i++) // unrolled loop
	{
		c->mov(qw0->r32(), SPU_OFF_32(gpr, op.ra, &v128::_u32, i));
		c->mov(asmjit::x86::ecx, SPU_OFF_32(gpr, op.rb, &v128::_u32, i));
		c->neg(asmjit::x86::ecx);
		c->shr(*qw0, asmjit::x86::cl);
		c->mov(SPU_OFF_32(gpr, op.rt, &v128::_u32, i), qw0->r32());
	}

	load_rcx();
}

void spu_recompiler::ROTMA(spu_opcode_t op)
{
	if (utils::has_avx2())
	{
		const XmmLink& va = XmmGet(op.ra, XmmType::Int);
		const XmmLink& vb = XmmGet(op.rb, XmmType::Int);
		const XmmLink& vt = XmmAlloc();
		c->psubd(vb, XmmConst(_mm_set1_epi32(1)));
		c->pandn(vb, XmmConst(_mm_set1_epi32(0x3f)));
		c->vpsravd(vt, va, vb);
		c->movdqa(SPU_OFF_128(gpr, op.rt), vt);
		return;
	}

	if (utils::has_xop())
	{
		const XmmLink& va = XmmGet(op.ra, XmmType::Int);
		const XmmLink& vb = XmmGet(op.rb, XmmType::Int);
		const XmmLink& vt = XmmAlloc();
		c->psubd(vb, XmmConst(_mm_set1_epi32(1)));
		c->pandn(vb, XmmConst(_mm_set1_epi32(0x3f)));
		c->pxor(vt, vt);
		c->pminud(vb, XmmConst(_mm_set1_epi32(31)));
		c->psubd(vt, vb);
		c->vpshad(vt, va, vt);
		c->movdqa(SPU_OFF_128(gpr, op.rt), vt);
		return;
	}

	save_rcx();

	for (u32 i = 0; i < 4; i++) // unrolled loop
	{
		c->movsxd(*qw0, SPU_OFF_32(gpr, op.ra, &v128::_u32, i));
		c->mov(asmjit::x86::ecx, SPU_OFF_32(gpr, op.rb, &v128::_u32, i));
		c->neg(asmjit::x86::ecx);
		c->sar(*qw0, asmjit::x86::cl);
		c->mov(SPU_OFF_32(gpr, op.rt, &v128::_u32, i), qw0->r32());
	}

	load_rcx();
}

void spu_recompiler::SHL(spu_opcode_t op)
{
	if (utils::has_avx2())
	{
		const XmmLink& va = XmmGet(op.ra, XmmType::Int);
		const XmmLink& vb = XmmGet(op.rb, XmmType::Int);
		const XmmLink& vt = XmmAlloc();
		c->pand(vb, XmmConst(_mm_set1_epi32(0x3f)));
		c->vpsllvd(vt, va, vb);
		c->movdqa(SPU_OFF_128(gpr, op.rt), vt);
		return;
	}

	if (utils::has_xop())
	{
		const XmmLink& va = XmmGet(op.ra, XmmType::Int);
		const XmmLink& vb = XmmGet(op.rb, XmmType::Int);
		const XmmLink& vt = XmmAlloc();
		c->pand(vb, XmmConst(_mm_set1_epi32(0x3f)));
		c->vpcmpgtd(vt, vb, XmmConst(_mm_set1_epi32(31)));
		c->vpshld(vb, va, vb);
		c->pandn(vt, vb);
		c->movdqa(SPU_OFF_128(gpr, op.rt), vt);
		return;
	}

	save_rcx();

	for (u32 i = 0; i < 4; i++) // unrolled loop
	{
		c->mov(qw0->r32(), SPU_OFF_32(gpr, op.ra, &v128::_u32, i));
		c->mov(asmjit::x86::ecx, SPU_OFF_32(gpr, op.rb, &v128::_u32, i));
		c->shl(*qw0, asmjit::x86::cl);
		c->mov(SPU_OFF_32(gpr, op.rt, &v128::_u32, i), qw0->r32());
	}

	load_rcx();
}

void spu_recompiler::ROTH(spu_opcode_t op) //nf
{
	if (utils::has_512())
	{
		const XmmLink& va = XmmGet(op.ra, XmmType::Int);
		const XmmLink& vb = XmmGet(op.rb, XmmType::Int);
		const XmmLink& vt = XmmAlloc();
		const XmmLink& v4 = XmmAlloc();
		c->vmovdqa(v4, XmmConst(_mm_set_epi32(0x0d0c0d0c, 0x09080908, 0x05040504, 0x01000100)));
		c->vpshufb(vt, va, v4); // duplicate low word
		c->vpsrld(va, va, 16);
		c->vpshufb(va, va, v4);
		c->vpsrld(v4, vb, 16);
		c->vprolvd(va, va, v4);
		c->vprolvd(vb, vt, vb);
		c->vpblendw(vt, vb, va, 0xaa);
		c->vmovdqa(SPU_OFF_128(gpr, op.rt), vt);
		return;
	}

	if (utils::has_xop())
	{
		const XmmLink& va = XmmGet(op.ra, XmmType::Int);
		const XmmLink& vb = XmmGet(op.rb, XmmType::Int);
		const XmmLink& vt = XmmAlloc();
		c->vprotw(vt, va, vb);
		c->movdqa(SPU_OFF_128(gpr, op.rt), vt);
		return;
	}

	save_rcx();

	for (u32 i = 0; i < 8; i++) // unrolled loop
	{
		c->movzx(qw0->r32(), SPU_OFF_16(gpr, op.ra, &v128::_u16, i));
		c->movzx(asmjit::x86::ecx, SPU_OFF_16(gpr, op.rb, &v128::_u16, i));
		c->rol(qw0->r16(), asmjit::x86::cl);
		c->mov(SPU_OFF_16(gpr, op.rt, &v128::_u16, i), qw0->r16());
	}

	load_rcx();
}

void spu_recompiler::ROTHM(spu_opcode_t op)
{
	if (utils::has_512())
	{
		const XmmLink& va = XmmGet(op.ra, XmmType::Int);
		const XmmLink& vb = XmmGet(op.rb, XmmType::Int);
		const XmmLink& vt = XmmAlloc();
		c->psubw(vb, XmmConst(_mm_set1_epi16(1)));
		c->pandn(vb, XmmConst(_mm_set1_epi16(0x1f)));
		c->vpsrlvw(vt, va, vb);
		c->movdqa(SPU_OFF_128(gpr, op.rt), vt);
		return;
	}

	if (utils::has_avx2())
	{
		const XmmLink& va = XmmGet(op.ra, XmmType::Int);
		const XmmLink& vb = XmmGet(op.rb, XmmType::Int);
		const XmmLink& vt = XmmAlloc();
		const XmmLink& v4 = XmmAlloc();
		const XmmLink& v5 = XmmAlloc();
		c->psubw(vb, XmmConst(_mm_set1_epi16(1)));
		c->pandn(vb, XmmConst(_mm_set1_epi16(0x1f)));
		c->movdqa(vt, XmmConst(_mm_set1_epi32(0xffff0000))); // mask: select high words
		c->vpsrld(v4, vb, 16);
		c->vpsubusw(v5, vb, vt); // clear high words (using saturation sub for throughput)
		c->vpandn(vb, vt, va); // clear high words
		c->vpsrlvd(va, va, v4);
		c->vpsrlvd(vb, vb, v5);
		c->vpblendw(vt, vb, va, 0xaa); // can use vpblendvb with 0xffff0000 mask (vt)
		c->movdqa(SPU_OFF_128(gpr, op.rt), vt);
		return;
	}

	if (utils::has_xop())
	{
		const XmmLink& va = XmmGet(op.ra, XmmType::Int);
		const XmmLink& vb = XmmGet(op.rb, XmmType::Int);
		const XmmLink& vt = XmmAlloc();
		c->psubw(vb, XmmConst(_mm_set1_epi16(1)));
		c->pandn(vb, XmmConst(_mm_set1_epi16(0x1f)));
		c->pxor(vt, vt);
		c->psubw(vt, vb);
		c->pcmpgtw(vb, XmmConst(_mm_set1_epi16(15)));
		c->vpshlw(vt, va, vt);
		c->vpandn(vt, vb, vt);
		c->movdqa(SPU_OFF_128(gpr, op.rt), vt);
		return;
	}

	save_rcx();

	for (u32 i = 0; i < 8; i++) // unrolled loop
	{
		c->movzx(qw0->r32(), SPU_OFF_16(gpr, op.ra, &v128::_u16, i));
		c->movzx(asmjit::x86::ecx, SPU_OFF_16(gpr, op.rb, &v128::_u16, i));
		c->neg(asmjit::x86::ecx);
		c->shr(qw0->r32(), asmjit::x86::cl);
		c->mov(SPU_OFF_16(gpr, op.rt, &v128::_u16, i), qw0->r16());
	}

	load_rcx();
}

void spu_recompiler::ROTMAH(spu_opcode_t op)
{
	if (utils::has_512())
	{
		const XmmLink& va = XmmGet(op.ra, XmmType::Int);
		const XmmLink& vb = XmmGet(op.rb, XmmType::Int);
		const XmmLink& vt = XmmAlloc();
		c->psubw(vb, XmmConst(_mm_set1_epi16(1)));
		c->pandn(vb, XmmConst(_mm_set1_epi16(0x1f)));
		c->vpsravw(vt, va, vb);
		c->movdqa(SPU_OFF_128(gpr, op.rt), vt);
		return;
	}

	if (utils::has_avx2())
	{
		const XmmLink& va = XmmGet(op.ra, XmmType::Int);
		const XmmLink& vb = XmmGet(op.rb, XmmType::Int);
		const XmmLink& vt = XmmAlloc();
		const XmmLink& v4 = XmmAlloc();
		const XmmLink& v5 = XmmAlloc();
		c->psubw(vb, XmmConst(_mm_set1_epi16(1)));
		c->movdqa(vt, XmmConst(_mm_set1_epi16(0x1f)));
		c->vpandn(v4, vb, vt);
		c->vpand(v5, vb, vt);
		c->movdqa(vt, XmmConst(_mm_set1_epi32(0x2f)));
		c->vpsrld(v4, v4, 16);
		c->vpsubusw(v5, vt, v5); // clear high word and add 16 to low word
		c->vpslld(vb, va, 16);
		c->vpsravd(va, va, v4);
		c->vpsravd(vb, vb, v5);
		c->vpblendw(vt, vb, va, 0xaa);
		c->movdqa(SPU_OFF_128(gpr, op.rt), vt);
		return;
	}

	if (utils::has_xop())
	{
		const XmmLink& va = XmmGet(op.ra, XmmType::Int);
		const XmmLink& vb = XmmGet(op.rb, XmmType::Int);
		const XmmLink& vt = XmmAlloc();
		c->psubw(vb, XmmConst(_mm_set1_epi16(1)));
		c->pandn(vb, XmmConst(_mm_set1_epi16(0x1f)));
		c->pxor(vt, vt);
		c->pminuw(vb, XmmConst(_mm_set1_epi16(15)));
		c->psubw(vt, vb);
		c->vpshaw(vt, va, vt);
		c->movdqa(SPU_OFF_128(gpr, op.rt), vt);
		return;
	}

	save_rcx();

	for (u32 i = 0; i < 8; i++) // unrolled loop
	{
		c->movsx(qw0->r32(), SPU_OFF_16(gpr, op.ra, &v128::_u16, i));
		c->movzx(asmjit::x86::ecx, SPU_OFF_16(gpr, op.rb, &v128::_u16, i));
		c->neg(asmjit::x86::ecx);
		c->sar(qw0->r32(), asmjit::x86::cl);
		c->mov(SPU_OFF_16(gpr, op.rt, &v128::_u16, i), qw0->r16());
	}

	load_rcx();
}

void spu_recompiler::SHLH(spu_opcode_t op)
{
	if (utils::has_512())
	{
		const XmmLink& va = XmmGet(op.ra, XmmType::Int);
		const XmmLink& vb = XmmGet(op.rb, XmmType::Int);
		const XmmLink& vt = XmmAlloc();
		c->pand(vb, XmmConst(_mm_set1_epi16(0x1f)));
		c->vpsllvw(vt, va, vb);
		c->movdqa(SPU_OFF_128(gpr, op.rt), vt);
		return;
	}

	if (utils::has_avx2())
	{
		const XmmLink& va = XmmGet(op.ra, XmmType::Int);
		const XmmLink& vb = XmmGet(op.rb, XmmType::Int);
		const XmmLink& vt = XmmAlloc();
		const XmmLink& v4 = XmmAlloc();
		const XmmLink& v5 = XmmAlloc();
		c->pand(vb, XmmConst(_mm_set1_epi16(0x1f)));
		c->movdqa(vt, XmmConst(_mm_set1_epi32(0xffff0000))); // mask: select high words
		c->vpsrld(v4, vb, 16);
		c->vpsubusw(v5, vb, vt); // clear high words (using saturation sub for throughput)
		c->vpand(vb, vt, va); // clear low words
		c->vpsllvd(va, va, v5);
		c->vpsllvd(vb, vb, v4);
		c->vpblendw(vt, vb, va, 0x55);
		c->movdqa(SPU_OFF_128(gpr, op.rt), vt);
		return;
	}

	if (utils::has_xop())
	{
		const XmmLink& va = XmmGet(op.ra, XmmType::Int);
		const XmmLink& vb = XmmGet(op.rb, XmmType::Int);
		const XmmLink& vt = XmmAlloc();
		c->pand(vb, XmmConst(_mm_set1_epi16(0x1f)));
		c->vpcmpgtw(vt, vb, XmmConst(_mm_set1_epi16(15)));
		c->vpshlw(vb, va, vb);
		c->pandn(vt, vb);
		c->movdqa(SPU_OFF_128(gpr, op.rt), vt);
		return;
	}

	save_rcx();

	for (u32 i = 0; i < 8; i++) // unrolled loop
	{
		c->movzx(qw0->r32(), SPU_OFF_16(gpr, op.ra, &v128::_u16, i));
		c->movzx(asmjit::x86::ecx, SPU_OFF_16(gpr, op.rb, &v128::_u16, i));
		c->shl(qw0->r32(), asmjit::x86::cl);
		c->mov(SPU_OFF_16(gpr, op.rt, &v128::_u16, i), qw0->r16());
	}

	load_rcx();
}

void spu_recompiler::ROTI(spu_opcode_t op)
{
	// rotate left
	const int s = op.i7 & 0x1f;

	if (utils::has_512())
	{
		const XmmLink& va = XmmGet(op.ra, XmmType::Int);
		c->vprold(va, va, s);
		c->movdqa(SPU_OFF_128(gpr, op.rt), va);
		return;
	}

	if (utils::has_xop())
	{
		const XmmLink& va = XmmGet(op.ra, XmmType::Int);
		c->vprotd(va, va, s);
		c->movdqa(SPU_OFF_128(gpr, op.rt), va);
		return;
	}

	const XmmLink& va = XmmGet(op.ra, XmmType::Int);
	const XmmLink& v1 = XmmAlloc();
	c->movdqa(v1, va);
	c->pslld(va, s);
	c->psrld(v1, 32 - s);
	c->por(va, v1);
	c->movdqa(SPU_OFF_128(gpr, op.rt), va);
}

void spu_recompiler::ROTMI(spu_opcode_t op)
{
	// shift right logical
	const int s = 0-op.i7 & 0x3f;
	const XmmLink& va = XmmGet(op.ra, XmmType::Int);
	c->psrld(va, s);
	c->movdqa(SPU_OFF_128(gpr, op.rt), va);
}

void spu_recompiler::ROTMAI(spu_opcode_t op)
{
	// shift right arithmetical
	const int s = 0-op.i7 & 0x3f;
	const XmmLink& va = XmmGet(op.ra, XmmType::Int);
	c->psrad(va, s);
	c->movdqa(SPU_OFF_128(gpr, op.rt), va);
}

void spu_recompiler::SHLI(spu_opcode_t op)
{
	// shift left
	const int s = op.i7 & 0x3f;
	const XmmLink& va = XmmGet(op.ra, XmmType::Int);
	c->pslld(va, s);
	c->movdqa(SPU_OFF_128(gpr, op.rt), va);
}

void spu_recompiler::ROTHI(spu_opcode_t op)
{
	// rotate left (halfword)
	const int s = op.i7 & 0xf;
	const XmmLink& va = XmmGet(op.ra, XmmType::Int);
	const XmmLink& v1 = XmmAlloc();
	c->movdqa(v1, va);
	c->psllw(va, s);
	c->psrlw(v1, 16 - s);
	c->por(va, v1);
	c->movdqa(SPU_OFF_128(gpr, op.rt), va);
}

void spu_recompiler::ROTHMI(spu_opcode_t op)
{
	// shift right logical
	const int s = 0-op.i7 & 0x1f;
	const XmmLink& va = XmmGet(op.ra, XmmType::Int);
	c->psrlw(va, s);
	c->movdqa(SPU_OFF_128(gpr, op.rt), va);
}

void spu_recompiler::ROTMAHI(spu_opcode_t op)
{
	// shift right arithmetical (halfword)
	const int s = 0-op.i7 & 0x1f;
	const XmmLink& va = XmmGet(op.ra, XmmType::Int);
	c->psraw(va, s);
	c->movdqa(SPU_OFF_128(gpr, op.rt), va);
}

void spu_recompiler::SHLHI(spu_opcode_t op)
{
	// shift left (halfword)
	const int s = op.i7 & 0x1f;
	const XmmLink& va = XmmGet(op.ra, XmmType::Int);
	c->psllw(va, s);
	c->movdqa(SPU_OFF_128(gpr, op.rt), va);
}

void spu_recompiler::A(spu_opcode_t op)
{
	const XmmLink& vb = XmmGet(op.rb, XmmType::Int);
	c->paddd(vb, SPU_OFF_128(gpr, op.ra));
	c->movdqa(SPU_OFF_128(gpr, op.rt), vb);
}

void spu_recompiler::AND(spu_opcode_t op)
{
	// and
	const XmmLink& vb = XmmGet(op.rb, XmmType::Int);
	c->pand(vb, SPU_OFF_128(gpr, op.ra));
	c->movdqa(SPU_OFF_128(gpr, op.rt), vb);
}

void spu_recompiler::CG(spu_opcode_t op)
{
	const XmmLink& va = XmmGet(op.ra, XmmType::Int);
	const XmmLink& vb = XmmGet(op.rb, XmmType::Int);
	const XmmLink& vi = XmmAlloc();

	if (utils::has_512())
	{
		c->vpaddd(vi, vb, va);
		c->vpternlogd(vi, va, vb, 0x8e /* A?andBC:orBC */);
		c->psrld(vi, 31);
		c->movdqa(SPU_OFF_128(gpr, op.rt), vi);
		return;
	}

	c->movdqa(vi, XmmConst(_mm_set1_epi32(0x80000000)));
	c->paddd(vb, va);
	c->pxor(va, vi);
	c->pxor(vb, vi);
	c->pcmpgtd(va, vb);
	c->psrld(va, 31);
	c->movdqa(SPU_OFF_128(gpr, op.rt), va);
}

void spu_recompiler::AH(spu_opcode_t op)
{
	const XmmLink& va = XmmGet(op.ra, XmmType::Int);
	c->paddw(va, SPU_OFF_128(gpr, op.rb));
	c->movdqa(SPU_OFF_128(gpr, op.rt), va);
}

void spu_recompiler::NAND(spu_opcode_t op)
{
	// nand
	const XmmLink& va = XmmGet(op.ra, XmmType::Int);

	if (utils::has_512())
	{
		c->vpternlogd(va, va, SPU_OFF_128(gpr, op.rb), 0x77 /* nandCB */);
		c->movdqa(SPU_OFF_128(gpr, op.rt), va);
		return;
	}

	c->pand(va, SPU_OFF_128(gpr, op.rb));
	c->pxor(va, XmmConst(_mm_set1_epi32(0xffffffff)));
	c->movdqa(SPU_OFF_128(gpr, op.rt), va);
}

void spu_recompiler::AVGB(spu_opcode_t op)
{
	const XmmLink& vb = XmmGet(op.rb, XmmType::Int);
	c->pavgb(vb, SPU_OFF_128(gpr, op.ra));
	c->movdqa(SPU_OFF_128(gpr, op.rt), vb);
}

void spu_recompiler::MTSPR(spu_opcode_t op)
{
	// Check SPUInterpreter for notes.
}

void spu_recompiler::WRCH(spu_opcode_t op)
{
	auto gate = [](SPUThread* _spu, u32 ch, u32 value)
	{
		if (_spu->set_ch_value(ch, value))
		{
			_spu->pc += 4;
		}
	};

	switch (op.ra)
	{
	case SPU_WrSRR0:
	{
		c->mov(*addr, SPU_OFF_32(gpr, op.rt, &v128::_u32, 3));
		c->mov(SPU_OFF_32(srr0), *addr);
		return;
	}
	case MFC_LSA:
	{
		c->mov(*addr, SPU_OFF_32(gpr, op.rt, &v128::_u32, 3));
		c->mov(SPU_OFF_32(ch_mfc_cmd, &spu_mfc_cmd::lsa), *addr);
		return;
	}
	case MFC_EAH:
	{
		c->mov(*addr, SPU_OFF_32(gpr, op.rt, &v128::_u32, 3));
		c->mov(SPU_OFF_32(ch_mfc_cmd, &spu_mfc_cmd::eah), *addr);
		return;
	}
	case MFC_EAL:
	{
		c->mov(*addr, SPU_OFF_32(gpr, op.rt, &v128::_u32, 3));
		c->mov(SPU_OFF_32(ch_mfc_cmd, &spu_mfc_cmd::eal), *addr);
		return;
	}
	case MFC_Size:
	{
		c->mov(*addr, SPU_OFF_32(gpr, op.rt, &v128::_u32, 3));
		c->mov(SPU_OFF_16(ch_mfc_cmd, &spu_mfc_cmd::size), addr->r16());
		return;
	}
	case MFC_TagID:
	{
		c->mov(*addr, SPU_OFF_32(gpr, op.rt, &v128::_u32, 3));
		c->mov(SPU_OFF_8(ch_mfc_cmd, &spu_mfc_cmd::tag), addr->r8());
		return;
	}
	case 69:
	{
		return;
	}
	}

	c->mov(SPU_OFF_32(pc), m_pos);
	c->mov(*ls, op.ra);
	c->mov(qw0->r32(), SPU_OFF_32(gpr, op.rt, &v128::_u32, 3));
	c->jmp(asmjit::imm_ptr<void(*)(SPUThread*, u32, u32)>(gate));
	m_pos = -1;
}

void spu_recompiler::BIZ(spu_opcode_t op)
{
	asmjit::Label branch_label = c->newLabel();
	c->cmp(SPU_OFF_32(gpr, op.rt, &v128::_u32, 3), 0);
	c->je(branch_label);

	after.emplace_back([=]
	{
		c->align(asmjit::kAlignCode, 16);
		c->bind(branch_label);
		c->mov(*addr, SPU_OFF_32(gpr, op.ra, &v128::_u32, 3));
		c->and_(*addr, 0x3fffc);
		branch_indirect(op);
	});
}

void spu_recompiler::BINZ(spu_opcode_t op)
{
	asmjit::Label branch_label = c->newLabel();
	c->cmp(SPU_OFF_32(gpr, op.rt, &v128::_u32, 3), 0);
	c->jne(branch_label);

	after.emplace_back([=]
	{
		c->align(asmjit::kAlignCode, 16);
		c->bind(branch_label);
		c->mov(*addr, SPU_OFF_32(gpr, op.ra, &v128::_u32, 3));
		c->and_(*addr, 0x3fffc);
		branch_indirect(op);
	});
}

void spu_recompiler::BIHZ(spu_opcode_t op)
{
	asmjit::Label branch_label = c->newLabel();
	c->cmp(SPU_OFF_16(gpr, op.rt, &v128::_u16, 6), 0);
	c->je(branch_label);

	after.emplace_back([=]
	{
		c->align(asmjit::kAlignCode, 16);
		c->bind(branch_label);
		c->mov(*addr, SPU_OFF_32(gpr, op.ra, &v128::_u32, 3));
		c->and_(*addr, 0x3fffc);
		branch_indirect(op);
	});
}

void spu_recompiler::BIHNZ(spu_opcode_t op)
{
	asmjit::Label branch_label = c->newLabel();
	c->cmp(SPU_OFF_16(gpr, op.rt, &v128::_u16, 6), 0);
	c->jne(branch_label);

	after.emplace_back([=]
	{
		c->align(asmjit::kAlignCode, 16);
		c->bind(branch_label);
		c->mov(*addr, SPU_OFF_32(gpr, op.ra, &v128::_u32, 3));
		c->and_(*addr, 0x3fffc);
		branch_indirect(op);
	});
}

void spu_recompiler::STOPD(spu_opcode_t op)
{
	fall(op);
}

void spu_recompiler::STQX(spu_opcode_t op)
{
	c->mov(*addr, SPU_OFF_32(gpr, op.ra, &v128::_u32, 3));
	c->add(*addr, SPU_OFF_32(gpr, op.rb, &v128::_u32, 3));
	c->and_(*addr, 0x3fff0);

	if (utils::has_ssse3())
	{
		const XmmLink& vt = XmmGet(op.rt, XmmType::Int);
		c->pshufb(vt, XmmConst(_mm_set_epi32(0x00010203, 0x04050607, 0x08090a0b, 0x0c0d0e0f)));
		c->movdqa(asmjit::x86::oword_ptr(*ls, addr->r64()), vt);
	}
	else
	{
		c->mov(*qw0, SPU_OFF_64(gpr, op.rt, &v128::_u64, 0));
		c->mov(*qw1, SPU_OFF_64(gpr, op.rt, &v128::_u64, 1));
		c->bswap(*qw0);
		c->bswap(*qw1);
		c->mov(asmjit::x86::qword_ptr(*ls, addr->r64(), 0, 0), *qw1);
		c->mov(asmjit::x86::qword_ptr(*ls, addr->r64(), 0, 8), *qw0);
	}
}

void spu_recompiler::BI(spu_opcode_t op)
{
	c->mov(*addr, SPU_OFF_32(gpr, op.ra, &v128::_u32, 3));
	c->and_(*addr, 0x3fffc);
	branch_indirect(op);
	m_pos = -1;
}

void spu_recompiler::BISL(spu_opcode_t op)
{
	c->mov(*addr, SPU_OFF_32(gpr, op.ra, &v128::_u32, 3));
	c->and_(*addr, 0x3fffc);
	const XmmLink& vr = XmmAlloc();
	c->movdqa(vr, XmmConst(_mm_set_epi32(spu_branch_target(m_pos + 4), 0, 0, 0)));
	c->movdqa(SPU_OFF_128(gpr, op.rt), vr);
	branch_indirect(op);
	m_pos = -1;
}

void spu_recompiler::IRET(spu_opcode_t op)
{
	c->mov(*addr, SPU_OFF_32(srr0));
	c->and_(*addr, 0x3fffc);
	branch_indirect(op);
	m_pos = -1;
}

void spu_recompiler::BISLED(spu_opcode_t op)
{
	fmt::throw_exception("Unimplemented instruction" HERE);
}

void spu_recompiler::HBR(spu_opcode_t op)
{
}

void spu_recompiler::GB(spu_opcode_t op)
{
	const XmmLink& va = XmmGet(op.ra, XmmType::Int);
	c->pslld(va, 31);
	c->movmskps(*addr, va);
	c->pxor(va, va);
	c->pinsrw(va, *addr, 6);
	c->movdqa(SPU_OFF_128(gpr, op.rt), va);
}

void spu_recompiler::GBH(spu_opcode_t op)
{
	const XmmLink& va = XmmGet(op.ra, XmmType::Int);
	c->psllw(va, 15);
	c->packsswb(va, XmmConst(_mm_setzero_si128()));
	c->pmovmskb(*addr, va);
	c->pxor(va, va);
	c->pinsrw(va, *addr, 6);
	c->movdqa(SPU_OFF_128(gpr, op.rt), va);
}

void spu_recompiler::GBB(spu_opcode_t op)
{
	const XmmLink& va = XmmGet(op.ra, XmmType::Int);
	c->psllq(va, 7);
	c->pmovmskb(*addr, va);
	c->pxor(va, va);
	c->pinsrw(va, *addr, 6);
	c->movdqa(SPU_OFF_128(gpr, op.rt), va);
}

void spu_recompiler::FSM(spu_opcode_t op)
{
	const XmmLink& vr = XmmAlloc();
	c->mov(*qw0, asmjit::imm_ptr((void*)g_spu_imm.fsm));
	c->mov(*addr, SPU_OFF_32(gpr, op.ra, &v128::_u32, 3));
	c->and_(*addr, 0xf);
	c->shl(*addr, 4);
	c->movdqa(vr, asmjit::x86::oword_ptr(*qw0, addr->r64()));
	c->movdqa(SPU_OFF_128(gpr, op.rt), vr);
}

void spu_recompiler::FSMH(spu_opcode_t op)
{
	const XmmLink& vr = XmmAlloc();
	c->mov(*qw0, asmjit::imm_ptr((void*)g_spu_imm.fsmh));
	c->mov(*addr, SPU_OFF_32(gpr, op.ra, &v128::_u32, 3));
	c->and_(*addr, 0xff);
	c->shl(*addr, 4);
	c->movdqa(vr, asmjit::x86::oword_ptr(*qw0, addr->r64()));
	c->movdqa(SPU_OFF_128(gpr, op.rt), vr);
}

void spu_recompiler::FSMB(spu_opcode_t op)
{
	const XmmLink& vr = XmmAlloc();
	c->mov(*qw0, asmjit::imm_ptr((void*)g_spu_imm.fsmb));
	c->mov(*addr, SPU_OFF_32(gpr, op.ra, &v128::_u32, 3));
	c->and_(*addr, 0xffff);
	c->shl(*addr, 4);
	c->movdqa(vr, asmjit::x86::oword_ptr(*qw0, addr->r64()));
	c->movdqa(SPU_OFF_128(gpr, op.rt), vr);
}

void spu_recompiler::FREST(spu_opcode_t op)
{
	const XmmLink& va = XmmGet(op.ra, XmmType::Float);
	c->rcpps(va, va);
	c->movaps(SPU_OFF_128(gpr, op.rt), va);
}

void spu_recompiler::FRSQEST(spu_opcode_t op)
{
	const XmmLink& va = XmmGet(op.ra, XmmType::Float);
	c->andps(va, XmmConst(_mm_set1_epi32(0x7fffffff))); // abs
	c->rsqrtps(va, va);
	c->movaps(SPU_OFF_128(gpr, op.rt), va);
}

void spu_recompiler::LQX(spu_opcode_t op)
{
	c->mov(*addr, SPU_OFF_32(gpr, op.ra, &v128::_u32, 3));
	c->add(*addr, SPU_OFF_32(gpr, op.rb, &v128::_u32, 3));
	c->and_(*addr, 0x3fff0);

	if (utils::has_ssse3())
	{
		const XmmLink& vt = XmmAlloc();
		c->movdqa(vt, asmjit::x86::oword_ptr(*ls, addr->r64()));
		c->pshufb(vt, XmmConst(_mm_set_epi32(0x00010203, 0x04050607, 0x08090a0b, 0x0c0d0e0f)));
		c->movdqa(SPU_OFF_128(gpr, op.rt), vt);
	}
	else
	{
		c->mov(*qw0, asmjit::x86::qword_ptr(*ls, addr->r64(), 0, 0));
		c->mov(*qw1, asmjit::x86::qword_ptr(*ls, addr->r64(), 0, 8));
		c->bswap(*qw0);
		c->bswap(*qw1);
		c->mov(SPU_OFF_64(gpr, op.rt, &v128::_u64, 0), *qw1);
		c->mov(SPU_OFF_64(gpr, op.rt, &v128::_u64, 1), *qw0);
	}
}

void spu_recompiler::ROTQBYBI(spu_opcode_t op)
{
	if (!utils::has_ssse3())
	{
		return fall(op);
	}

	const XmmLink& va = XmmGet(op.ra, XmmType::Int);
	c->mov(*qw0, asmjit::imm_ptr((void*)g_spu_imm.rldq_pshufb));
	c->mov(*addr, SPU_OFF_32(gpr, op.rb, &v128::_u32, 3));
	c->and_(*addr, 0xf << 3);
	c->pshufb(va, asmjit::x86::oword_ptr(*qw0, addr->r64(), 1));
	c->movdqa(SPU_OFF_128(gpr, op.rt), va);
}

void spu_recompiler::ROTQMBYBI(spu_opcode_t op)
{
	if (!utils::has_ssse3())
	{
		return fall(op);
	}

	const XmmLink& va = XmmGet(op.ra, XmmType::Int);
	c->mov(*qw0, asmjit::imm_ptr((void*)g_spu_imm.srdq_pshufb));
	c->mov(*addr, SPU_OFF_32(gpr, op.rb, &v128::_u32, 3));
	c->and_(*addr, 0x1f << 3);
	c->pshufb(va, asmjit::x86::oword_ptr(*qw0, addr->r64(), 1));
	c->movdqa(SPU_OFF_128(gpr, op.rt), va);
}

void spu_recompiler::SHLQBYBI(spu_opcode_t op)
{
	if (!utils::has_ssse3())
	{
		return fall(op);
	}

	const XmmLink& va = XmmGet(op.ra, XmmType::Int);
	c->mov(*qw0, asmjit::imm_ptr((void*)g_spu_imm.sldq_pshufb));
	c->mov(*addr, SPU_OFF_32(gpr, op.rb, &v128::_u32, 3));
	c->and_(*addr, 0x1f << 3);
	c->pshufb(va, asmjit::x86::oword_ptr(*qw0, addr->r64(), 1));
	c->movdqa(SPU_OFF_128(gpr, op.rt), va);
}

void spu_recompiler::CBX(spu_opcode_t op)
{
	c->mov(*addr, SPU_OFF_32(gpr, op.rb, &v128::_u32, 3));
	c->add(*addr, SPU_OFF_32(gpr, op.ra, &v128::_u32, 3));
	c->not_(*addr);
	c->and_(*addr, 0xf);

	const XmmLink& vr = XmmAlloc();
	c->movdqa(vr, XmmConst(_mm_set_epi32(0x10111213, 0x14151617, 0x18191a1b, 0x1c1d1e1f)));
	c->movdqa(SPU_OFF_128(gpr, op.rt), vr);
	c->mov(asmjit::x86::byte_ptr(*cpu, addr->r64(), 0, offset32(&SPUThread::gpr, op.rt)), 0x03);
}

void spu_recompiler::CHX(spu_opcode_t op)
{
	c->mov(*addr, SPU_OFF_32(gpr, op.rb, &v128::_u32, 3));
	c->add(*addr, SPU_OFF_32(gpr, op.ra, &v128::_u32, 3));
	c->not_(*addr);
	c->and_(*addr, 0xe);

	const XmmLink& vr = XmmAlloc();
	c->movdqa(vr, XmmConst(_mm_set_epi32(0x10111213, 0x14151617, 0x18191a1b, 0x1c1d1e1f)));
	c->movdqa(SPU_OFF_128(gpr, op.rt), vr);
	c->mov(asmjit::x86::word_ptr(*cpu, addr->r64(), 0, offset32(&SPUThread::gpr, op.rt)), 0x0203);
}

void spu_recompiler::CWX(spu_opcode_t op)
{
	c->mov(*addr, SPU_OFF_32(gpr, op.rb, &v128::_u32, 3));
	c->add(*addr, SPU_OFF_32(gpr, op.ra, &v128::_u32, 3));
	c->not_(*addr);
	c->and_(*addr, 0xc);

	const XmmLink& vr = XmmAlloc();
	c->movdqa(vr, XmmConst(_mm_set_epi32(0x10111213, 0x14151617, 0x18191a1b, 0x1c1d1e1f)));
	c->movdqa(SPU_OFF_128(gpr, op.rt), vr);
	c->mov(asmjit::x86::dword_ptr(*cpu, addr->r64(), 0, offset32(&SPUThread::gpr, op.rt)), 0x00010203);
}

void spu_recompiler::CDX(spu_opcode_t op)
{
	c->mov(*addr, SPU_OFF_32(gpr, op.rb, &v128::_u32, 3));
	c->add(*addr, SPU_OFF_32(gpr, op.ra, &v128::_u32, 3));
	c->not_(*addr);
	c->and_(*addr, 0x8);

	const XmmLink& vr = XmmAlloc();
	c->movdqa(vr, XmmConst(_mm_set_epi32(0x10111213, 0x14151617, 0x18191a1b, 0x1c1d1e1f)));
	c->movdqa(SPU_OFF_128(gpr, op.rt), vr);
	c->mov(*qw0, asmjit::imm_u(0x0001020304050607));
	c->mov(asmjit::x86::qword_ptr(*cpu, addr->r64(), 0, offset32(&SPUThread::gpr, op.rt)), *qw0);
}

void spu_recompiler::ROTQBI(spu_opcode_t op)
{
	const XmmLink& va = XmmGet(op.ra, XmmType::Int);
	const XmmLink& vb = XmmGet(op.rb, XmmType::Int);
	const XmmLink& vt = XmmAlloc();
	const XmmLink& v4 = XmmAlloc();
	c->psrldq(vb, 12);
	c->pand(vb, XmmConst(_mm_set_epi64x(0, 7)));
	c->movdqa(v4, XmmConst(_mm_set_epi64x(0, 64)));
	c->pshufd(vt, va, 0x4e);
	c->psubq(v4, vb);
	c->psllq(va, vb);
	c->psrlq(vt, v4);
	c->por(vt, va);
	c->movdqa(SPU_OFF_128(gpr, op.rt), vt);
}

void spu_recompiler::ROTQMBI(spu_opcode_t op)
{
	const XmmLink& va = XmmGet(op.ra, XmmType::Int);
	const XmmLink& vb = XmmAlloc();
	const XmmLink& vt = XmmGet(op.rb, XmmType::Int);
	const XmmLink& v4 = XmmAlloc();
	c->psrldq(vt, 12);
	c->pxor(vb, vb);
	c->psubq(vb, vt);
	c->pand(vb, XmmConst(_mm_set_epi64x(0, 7)));
	c->movdqa(v4, XmmConst(_mm_set_epi64x(0, 64)));
	c->movdqa(vt, va);
	c->psrldq(vt, 8);
	c->psubq(v4, vb);
	c->psrlq(va, vb);
	c->psllq(vt, v4);
	c->por(vt, va);
	c->movdqa(SPU_OFF_128(gpr, op.rt), vt);
}

void spu_recompiler::SHLQBI(spu_opcode_t op)
{
	const XmmLink& va = XmmGet(op.ra, XmmType::Int);
	const XmmLink& vb = XmmGet(op.rb, XmmType::Int);
	const XmmLink& vt = XmmAlloc();
	const XmmLink& v4 = XmmAlloc();
	c->psrldq(vb, 12);
	c->pand(vb, XmmConst(_mm_set_epi64x(0, 7)));
	c->movdqa(v4, XmmConst(_mm_set_epi64x(0, 64)));
	c->movdqa(vt, va);
	c->pslldq(vt, 8);
	c->psubq(v4, vb);
	c->psllq(va, vb);
	c->psrlq(vt, v4);
	c->por(vt, va);
	c->movdqa(SPU_OFF_128(gpr, op.rt), vt);
}

void spu_recompiler::ROTQBY(spu_opcode_t op)
{
	if (!utils::has_ssse3())
	{
		return fall(op);
	}

	const XmmLink& va = XmmGet(op.ra, XmmType::Int);
	c->mov(*qw0, asmjit::imm_ptr((void*)g_spu_imm.rldq_pshufb));
	c->mov(*addr, SPU_OFF_32(gpr, op.rb, &v128::_u32, 3));
	c->and_(*addr, 0xf);
	c->shl(*addr, 4);
	c->pshufb(va, asmjit::x86::oword_ptr(*qw0, addr->r64()));
	c->movdqa(SPU_OFF_128(gpr, op.rt), va);
}

void spu_recompiler::ROTQMBY(spu_opcode_t op)
{
	if (!utils::has_ssse3())
	{
		return fall(op);
	}

	const XmmLink& va = XmmGet(op.ra, XmmType::Int);
	c->mov(*qw0, asmjit::imm_ptr((void*)g_spu_imm.srdq_pshufb));
	c->mov(*addr, SPU_OFF_32(gpr, op.rb, &v128::_u32, 3));
	c->and_(*addr, 0x1f);
	c->shl(*addr, 4);
	c->pshufb(va, asmjit::x86::oword_ptr(*qw0, addr->r64()));
	c->movdqa(SPU_OFF_128(gpr, op.rt), va);
}

void spu_recompiler::SHLQBY(spu_opcode_t op)
{
	if (!utils::has_ssse3())
	{
		return fall(op);
	}

	const XmmLink& va = XmmGet(op.ra, XmmType::Int);
	c->mov(*qw0, asmjit::imm_ptr((void*)g_spu_imm.sldq_pshufb));
	c->mov(*addr, SPU_OFF_32(gpr, op.rb, &v128::_u32, 3));
	c->and_(*addr, 0x1f);
	c->shl(*addr, 4);
	c->pshufb(va, asmjit::x86::oword_ptr(*qw0, addr->r64()));
	c->movdqa(SPU_OFF_128(gpr, op.rt), va);
}

void spu_recompiler::ORX(spu_opcode_t op)
{
	const XmmLink& va = XmmGet(op.ra, XmmType::Int);
	const XmmLink& v1 = XmmAlloc();
	c->pshufd(v1, va, 0xb1);
	c->por(va, v1);
	c->pshufd(v1, va, 0x4e);
	c->por(va, v1);
	c->pslldq(va, 12);
	c->movdqa(SPU_OFF_128(gpr, op.rt), va);
}

void spu_recompiler::CBD(spu_opcode_t op)
{
	//if (op.ra == 1)
	//{
	//	// assuming that SP % 16 is always zero
	//	const XmmLink& vr = XmmAlloc();
	//	v128 value = v128::fromV(_mm_set_epi32(0x10111213, 0x14151617, 0x18191a1b, 0x1c1d1e1f));
	//	value.u8r[op.i7 & 0xf] = 0x03;
	//	c->movdqa(vr, XmmConst(value));
	//	c->movdqa(SPU_OFF_128(gpr, op.rt), vr);
	//	return;
	//}

	c->mov(*addr, SPU_OFF_32(gpr, op.ra, &v128::_u32, 3));
	if (op.i7) c->add(*addr, op.i7);
	c->not_(*addr);
	c->and_(*addr, 0xf);

	const XmmLink& vr = XmmAlloc();
	c->movdqa(vr, XmmConst(_mm_set_epi32(0x10111213, 0x14151617, 0x18191a1b, 0x1c1d1e1f)));
	c->movdqa(SPU_OFF_128(gpr, op.rt), vr);
	c->mov(asmjit::x86::byte_ptr(*cpu, addr->r64(), 0, offset32(&SPUThread::gpr, op.rt)), 0x03);
}

void spu_recompiler::CHD(spu_opcode_t op)
{
	//if (op.ra == 1)
	//{
	//	// assuming that SP % 16 is always zero
	//	const XmmLink& vr = XmmAlloc();
	//	v128 value = v128::fromV(_mm_set_epi32(0x10111213, 0x14151617, 0x18191a1b, 0x1c1d1e1f));
	//	value.u16r[(op.i7 >> 1) & 0x7] = 0x0203;
	//	c->movdqa(vr, XmmConst(value));
	//	c->movdqa(SPU_OFF_128(gpr, op.rt), vr);
	//	return;
	//}

	c->mov(*addr, SPU_OFF_32(gpr, op.ra, &v128::_u32, 3));
	if (op.i7) c->add(*addr, op.i7);
	c->not_(*addr);
	c->and_(*addr, 0xe);

	const XmmLink& vr = XmmAlloc();
	c->movdqa(vr, XmmConst(_mm_set_epi32(0x10111213, 0x14151617, 0x18191a1b, 0x1c1d1e1f)));
	c->movdqa(SPU_OFF_128(gpr, op.rt), vr);
	c->mov(asmjit::x86::word_ptr(*cpu, addr->r64(), 0, offset32(&SPUThread::gpr, op.rt)), 0x0203);
}

void spu_recompiler::CWD(spu_opcode_t op)
{
	//if (op.ra == 1)
	//{
	//	// assuming that SP % 16 is always zero
	//	const XmmLink& vr = XmmAlloc();
	//	v128 value = v128::fromV(_mm_set_epi32(0x10111213, 0x14151617, 0x18191a1b, 0x1c1d1e1f));
	//	value.u32r[(op.i7 >> 2) & 0x3] = 0x00010203;
	//	c->movdqa(vr, XmmConst(value));
	//	c->movdqa(SPU_OFF_128(gpr, op.rt), vr);
	//	return;
	//}

	c->mov(*addr, SPU_OFF_32(gpr, op.ra, &v128::_u32, 3));
	if (op.i7) c->add(*addr, op.i7);
	c->not_(*addr);
	c->and_(*addr, 0xc);

	const XmmLink& vr = XmmAlloc();
	c->movdqa(vr, XmmConst(_mm_set_epi32(0x10111213, 0x14151617, 0x18191a1b, 0x1c1d1e1f)));
	c->movdqa(SPU_OFF_128(gpr, op.rt), vr);
	c->mov(asmjit::x86::dword_ptr(*cpu, addr->r64(), 0, offset32(&SPUThread::gpr, op.rt)), 0x00010203);
}

void spu_recompiler::CDD(spu_opcode_t op)
{
	//if (op.ra == 1)
	//{
	//	// assuming that SP % 16 is always zero
	//	const XmmLink& vr = XmmAlloc();
	//	v128 value = v128::fromV(_mm_set_epi32(0x10111213, 0x14151617, 0x18191a1b, 0x1c1d1e1f));
	//	value.u64r[(op.i7 >> 3) & 0x1] = 0x0001020304050607ull;
	//	c->movdqa(vr, XmmConst(value));
	//	c->movdqa(SPU_OFF_128(gpr, op.rt), vr);
	//	return;
	//}

	c->mov(*addr, SPU_OFF_32(gpr, op.ra, &v128::_u32, 3));
	if (op.i7) c->add(*addr, op.i7);
	c->not_(*addr);
	c->and_(*addr, 0x8);

	const XmmLink& vr = XmmAlloc();
	c->movdqa(vr, XmmConst(_mm_set_epi32(0x10111213, 0x14151617, 0x18191a1b, 0x1c1d1e1f)));
	c->movdqa(SPU_OFF_128(gpr, op.rt), vr);
	c->mov(*qw0, asmjit::imm_u(0x0001020304050607));
	c->mov(asmjit::x86::qword_ptr(*cpu, addr->r64(), 0, offset32(&SPUThread::gpr, op.rt)), *qw0);
}

void spu_recompiler::ROTQBII(spu_opcode_t op)
{
	const XmmLink& va = XmmGet(op.ra, XmmType::Int);
	const XmmLink& vt = XmmAlloc();
	c->pshufd(vt, va, 0x4e); // swap 64-bit parts
	c->psllq(va, (op.i7 & 0x7));
	c->psrlq(vt, 64 - (op.i7 & 0x7));
	c->por(vt, va);
	c->movdqa(SPU_OFF_128(gpr, op.rt), vt);
}

void spu_recompiler::ROTQMBII(spu_opcode_t op)
{
	const XmmLink& va = XmmGet(op.ra, XmmType::Int);
	const XmmLink& vt = XmmAlloc();
	c->movdqa(vt, va);
	c->psrldq(vt, 8);
	c->psrlq(va, ((0 - op.i7) & 0x7));
	c->psllq(vt, 64 - ((0 - op.i7) & 0x7));
	c->por(vt, va);
	c->movdqa(SPU_OFF_128(gpr, op.rt), vt);
}

void spu_recompiler::SHLQBII(spu_opcode_t op)
{
	const XmmLink& va = XmmGet(op.ra, XmmType::Int);
	const XmmLink& vt = XmmAlloc();
	c->movdqa(vt, va);
	c->pslldq(vt, 8);
	c->psllq(va, (op.i7 & 0x7));
	c->psrlq(vt, 64 - (op.i7 & 0x7));
	c->por(vt, va);
	c->movdqa(SPU_OFF_128(gpr, op.rt), vt);
}

void spu_recompiler::ROTQBYI(spu_opcode_t op)
{
	const int s = op.i7 & 0xf;
	const XmmLink& va = XmmGet(op.ra, XmmType::Int);
	const XmmLink& v2 = XmmAlloc();

	if (s == 0)
	{
	}
	else if (s == 4 || s == 8 || s == 12)
	{
		c->pshufd(va, va, ::rol8(0xE4, s / 2));
	}
	else if (utils::has_ssse3())
	{
		c->palignr(va, va, 16 - s);
	}
	else
	{
		c->movdqa(v2, va);
		c->psrldq(va, 16 - s);
		c->pslldq(v2, s);
		c->por(va, v2);
	}

	c->movdqa(SPU_OFF_128(gpr, op.rt), va);
}

void spu_recompiler::ROTQMBYI(spu_opcode_t op)
{
	const int s = 0-op.i7 & 0x1f;
	const XmmLink& va = XmmGet(op.ra, XmmType::Int);
	c->psrldq(va, s);
	c->movdqa(SPU_OFF_128(gpr, op.rt), va);
}

void spu_recompiler::SHLQBYI(spu_opcode_t op)
{
	const int s = op.i7 & 0x1f;
	const XmmLink& va = XmmGet(op.ra, XmmType::Int);
	c->pslldq(va, s);
	c->movdqa(SPU_OFF_128(gpr, op.rt), va);
}

void spu_recompiler::NOP(spu_opcode_t op)
{
}

void spu_recompiler::CGT(spu_opcode_t op)
{
	const XmmLink& va = XmmGet(op.ra, XmmType::Int);
	c->pcmpgtd(va, SPU_OFF_128(gpr, op.rb));
	c->movdqa(SPU_OFF_128(gpr, op.rt), va);
}

void spu_recompiler::XOR(spu_opcode_t op)
{
	// xor
	const XmmLink& va = XmmGet(op.ra, XmmType::Int);
	c->pxor(va, SPU_OFF_128(gpr, op.rb));
	c->movdqa(SPU_OFF_128(gpr, op.rt), va);
}

void spu_recompiler::CGTH(spu_opcode_t op)
{
	const XmmLink& va = XmmGet(op.ra, XmmType::Int);
	c->pcmpgtw(va, SPU_OFF_128(gpr, op.rb));
	c->movdqa(SPU_OFF_128(gpr, op.rt), va);
}

void spu_recompiler::EQV(spu_opcode_t op)
{
	const XmmLink& vb = XmmGet(op.rb, XmmType::Int);

	if (utils::has_512())
	{
		c->vpternlogd(vb, vb, SPU_OFF_128(gpr, op.ra), 0x99 /* xnorCB */);
		c->movdqa(SPU_OFF_128(gpr, op.rt), vb);
		return;
	}

	c->pxor(vb, XmmConst(_mm_set1_epi32(0xffffffff)));
	c->pxor(vb, SPU_OFF_128(gpr, op.ra));
	c->movdqa(SPU_OFF_128(gpr, op.rt), vb);
}

void spu_recompiler::CGTB(spu_opcode_t op)
{
	const XmmLink& va = XmmGet(op.ra, XmmType::Int);
	c->pcmpgtb(va, SPU_OFF_128(gpr, op.rb));
	c->movdqa(SPU_OFF_128(gpr, op.rt), va);
}

void spu_recompiler::SUMB(spu_opcode_t op)
{
	const XmmLink& va = XmmGet(op.ra, XmmType::Int);
	const XmmLink& vb = XmmGet(op.rb, XmmType::Int);
	const XmmLink& v1 = XmmAlloc();
	const XmmLink& v2 = XmmAlloc();
	c->movdqa(v2, XmmConst(_mm_set1_epi16(0xff)));
	c->movdqa(v1, va);
	c->psrlw(va, 8);
	c->pand(v1, v2);
	c->pand(v2, vb);
	c->psrlw(vb, 8);
	c->paddw(va, v1);
	c->paddw(vb, v2);
	c->movdqa(v2, XmmConst(_mm_set1_epi32(0xffff)));
	c->movdqa(v1, va);
	c->psrld(va, 16);
	c->pand(v1, v2);
	c->pandn(v2, vb);
	c->pslld(vb, 16);
	c->paddw(va, v1);
	c->paddw(vb, v2);
	c->por(va, vb);
	c->movdqa(SPU_OFF_128(gpr, op.rt), va);
}

//HGT uses signed values.  HLGT uses unsigned values
void spu_recompiler::HGT(spu_opcode_t op)
{
	c->mov(*addr, SPU_OFF_32(gpr, op.ra, &v128::_s32, 3));
	c->cmp(*addr, SPU_OFF_32(gpr, op.rb, &v128::_s32, 3));
	c->jg(halt(m_pos));
}

void spu_recompiler::CLZ(spu_opcode_t op)
{
	if (utils::has_512())
	{
		const XmmLink& va = XmmGet(op.ra, XmmType::Int);
		const XmmLink& vt = XmmAlloc();
		c->vplzcntd(vt, va);
		c->movdqa(SPU_OFF_128(gpr, op.rt), vt);
		return;
	}

	c->mov(qw0->r32(), 32 + 31);
	for (u32 i = 0; i < 4; i++) // unrolled loop
	{
		c->bsr(*addr, SPU_OFF_32(gpr, op.ra, &v128::_u32, i));
		c->cmovz(*addr, qw0->r32());
		c->xor_(*addr, 31);
		c->mov(SPU_OFF_32(gpr, op.rt, &v128::_u32, i), *addr);
	}
}

void spu_recompiler::XSWD(spu_opcode_t op)
{
	c->movsxd(*qw0, SPU_OFF_32(gpr, op.ra, &v128::_s32, 0));
	c->movsxd(*qw1, SPU_OFF_32(gpr, op.ra, &v128::_s32, 2));
	c->mov(SPU_OFF_64(gpr, op.rt, &v128::_s64, 0), *qw0);
	c->mov(SPU_OFF_64(gpr, op.rt, &v128::_s64, 1), *qw1);
}

void spu_recompiler::XSHW(spu_opcode_t op)
{
	const XmmLink& va = XmmGet(op.ra, XmmType::Int);
	c->pslld(va, 16);
	c->psrad(va, 16);
	c->movdqa(SPU_OFF_128(gpr, op.rt), va);
}

void spu_recompiler::CNTB(spu_opcode_t op)
{
	const XmmLink& va = XmmGet(op.ra, XmmType::Int);
	const XmmLink& v1 = XmmAlloc();
	const XmmLink& vm = XmmAlloc();
	c->movdqa(vm, XmmConst(_mm_set1_epi8(0x55)));
	c->movdqa(v1, va);
	c->pand(va, vm);
	c->psrlq(v1, 1);
	c->pand(v1, vm);
	c->paddb(va, v1);
	c->movdqa(vm, XmmConst(_mm_set1_epi8(0x33)));
	c->movdqa(v1, va);
	c->pand(va, vm);
	c->psrlq(v1, 2);
	c->pand(v1, vm);
	c->paddb(va, v1);
	c->movdqa(vm, XmmConst(_mm_set1_epi8(0x0f)));
	c->movdqa(v1, va);
	c->pand(va, vm);
	c->psrlq(v1, 4);
	c->pand(v1, vm);
	c->paddb(va, v1);
	c->movdqa(SPU_OFF_128(gpr, op.rt), va);
}

void spu_recompiler::XSBH(spu_opcode_t op)
{
	const XmmLink& va = XmmGet(op.ra, XmmType::Int);
	c->psllw(va, 8);
	c->psraw(va, 8);
	c->movdqa(SPU_OFF_128(gpr, op.rt), va);
}

void spu_recompiler::CLGT(spu_opcode_t op)
{
	// compare if-greater-than
	const XmmLink& va = XmmGet(op.ra, XmmType::Int);
	const XmmLink& vi = XmmAlloc();
	c->movdqa(vi, XmmConst(_mm_set1_epi32(0x80000000)));
	c->pxor(va, vi);
	c->pxor(vi, SPU_OFF_128(gpr, op.rb));
	c->pcmpgtd(va, vi);
	c->movdqa(SPU_OFF_128(gpr, op.rt), va);
}

void spu_recompiler::ANDC(spu_opcode_t op)
{
	// and not
	const XmmLink& vb = XmmGet(op.rb, XmmType::Int);
	c->pandn(vb, SPU_OFF_128(gpr, op.ra));
	c->movdqa(SPU_OFF_128(gpr, op.rt), vb);
}

void spu_recompiler::FCGT(spu_opcode_t op)
{
	const auto last_exp_bit = XmmConst(_mm_set1_epi32(0x00800000));
	const auto all_exp_bits = XmmConst(_mm_set1_epi32(0x7f800000));

	const XmmLink& tmp0 = XmmAlloc();
	const XmmLink& tmp1 = XmmAlloc();
	const XmmLink& tmp2 = XmmAlloc();
	const XmmLink& tmp3 = XmmAlloc();
	const XmmLink& tmpv = XmmAlloc();

	c->pxor(tmp0, tmp0);
	c->pxor(tmp1, tmp1);
	c->cmpps(tmp0, SPU_OFF_128(gpr, op.ra), 3);  //tmp0 is true if a is extended (nan/inf)
	c->cmpps(tmp1, SPU_OFF_128(gpr, op.rb), 3);  //tmp1 is true if b is extended (nan/inf)

	//compute lower a and b
	c->movaps(tmp2, last_exp_bit);
	c->movaps(tmp3, last_exp_bit);
	c->pandn(tmp2, SPU_OFF_128(gpr, op.ra));  //tmp2 = lowered_a
	c->pandn(tmp3, SPU_OFF_128(gpr, op.rb));  //tmp3 = lowered_b

	//lower a if extended
	c->movaps(tmpv, tmp0);
	c->pand(tmpv, tmp2);
	c->pandn(tmp0, SPU_OFF_128(gpr, op.ra));
	c->orps(tmp0, tmpv);

	//lower b if extended
	c->movaps(tmpv, tmp1);
	c->pand(tmpv, tmp3);
	c->pandn(tmp1, SPU_OFF_128(gpr, op.rb));
	c->orps(tmp1, tmpv);

	//flush to 0 if denormalized
	c->pxor(tmpv, tmpv);
	c->movaps(tmp2, SPU_OFF_128(gpr, op.ra));
	c->movaps(tmp3, SPU_OFF_128(gpr, op.rb));
	c->andps(tmp2, all_exp_bits);
	c->andps(tmp3, all_exp_bits);
	c->cmpps(tmp2, tmpv, 0);
	c->cmpps(tmp3, tmpv, 0);
	c->pandn(tmp2, tmp0);
	c->pandn(tmp3, tmp1);

	c->cmpps(tmp3, tmp2, 1);
	c->movaps(SPU_OFF_128(gpr, op.rt), tmp3);
}

void spu_recompiler::DFCGT(spu_opcode_t op)
{
	fmt::throw_exception("Unexpected instruction" HERE);
}

void spu_recompiler::FA(spu_opcode_t op)
{
	const XmmLink& va = XmmGet(op.ra, XmmType::Float);
	c->addps(va, SPU_OFF_128(gpr, op.rb));
	c->movaps(SPU_OFF_128(gpr, op.rt), va);
}

void spu_recompiler::FS(spu_opcode_t op)
{
	const XmmLink& va = XmmGet(op.ra, XmmType::Float);
	c->subps(va, SPU_OFF_128(gpr, op.rb));
	c->movaps(SPU_OFF_128(gpr, op.rt), va);
}

void spu_recompiler::FM(spu_opcode_t op)
{
	const auto sign_bits = XmmConst(_mm_set1_epi32(0x80000000));
	const auto all_exp_bits = XmmConst(_mm_set1_epi32(0x7f800000));

	const XmmLink& tmp0 = XmmAlloc();
	const XmmLink& tmp1 = XmmAlloc();
	const XmmLink& tmp2 = XmmAlloc();
	const XmmLink& tmp3 = XmmAlloc();
	const XmmLink& tmp4 = XmmGet(op.ra, XmmType::Float);
	const XmmLink& tmp5 = XmmGet(op.rb, XmmType::Float);

	//check denormals
	c->pxor(tmp0, tmp0);
	c->movaps(tmp1, all_exp_bits);
	c->movaps(tmp2, all_exp_bits);
	c->andps(tmp1, tmp4);
	c->andps(tmp2, tmp5);
	c->cmpps(tmp1, tmp0, 0);
	c->cmpps(tmp2, tmp0, 0);
	c->orps(tmp1, tmp2);  //denormal operand mask

	//compute result with flushed denormal inputs
	c->movaps(tmp2, tmp4);
	c->mulps(tmp2, tmp5); //primary result
	c->movaps(tmp3, tmp2);
	c->andps(tmp3, all_exp_bits);
	c->cmpps(tmp3, tmp0, 0); //denom mask from result
	c->orps(tmp3, tmp1);
	c->andnps(tmp3, tmp2); //flushed result

	//compute results for the extended path
	c->andps(tmp2, all_exp_bits);
	c->cmpps(tmp2, all_exp_bits, 0); //extended mask
	c->movaps(tmp4, sign_bits);
	c->movaps(tmp5, sign_bits);
	c->movaps(tmp0, sign_bits);
	c->andps(tmp4, SPU_OFF_128(gpr, op.ra));
	c->andps(tmp5, SPU_OFF_128(gpr, op.rb));
	c->xorps(tmp4, tmp5); //sign mask
	c->pandn(tmp0, tmp2);
	c->orps(tmp4, tmp0); //add result sign back to original extended value
	c->movaps(tmp5, tmp1); //denormal mask (operands)
	c->andnps(tmp5, tmp4); //max_float with sign bit (nan/-nan) where not denormal or zero

	//select result
	c->movaps(tmp0, tmp2);
	c->andnps(tmp0, tmp3);
	c->andps(tmp2, tmp5);
	c->orps(tmp0, tmp2);
	c->movaps(SPU_OFF_128(gpr, op.rt), tmp0);
}

void spu_recompiler::CLGTH(spu_opcode_t op)
{
	// compare if-greater-than
	const XmmLink& va = XmmGet(op.ra, XmmType::Int);
	const XmmLink& vi = XmmAlloc();
	c->movdqa(vi, XmmConst(_mm_set1_epi16(INT16_MIN)));
	c->pxor(va, vi);
	c->pxor(vi, SPU_OFF_128(gpr, op.rb));
	c->pcmpgtw(va, vi);
	c->movdqa(SPU_OFF_128(gpr, op.rt), va);
}

void spu_recompiler::ORC(spu_opcode_t op)
{
	const XmmLink& vb = XmmGet(op.rb, XmmType::Int);

	if (utils::has_512())
	{
		c->vpternlogd(vb, vb, SPU_OFF_128(gpr, op.ra), 0xbb /* orC!B */);
		c->movdqa(SPU_OFF_128(gpr, op.rt), vb);
		return;
	}

	c->pxor(vb, XmmConst(_mm_set1_epi32(0xffffffff)));
	c->por(vb, SPU_OFF_128(gpr, op.ra));
	c->movdqa(SPU_OFF_128(gpr, op.rt), vb);
}

void spu_recompiler::FCMGT(spu_opcode_t op)
{
	// reverted less-than
	// since comparison is absoulte, a > b if a is extended and b is not extended
	// flush denormals to zero to make zero == zero work
	const auto last_exp_bit = XmmConst(_mm_set1_epi32(0x00800000));
	const auto all_exp_bits = XmmConst(_mm_set1_epi32(0x7f800000));
	const auto remove_sign_bits = XmmConst(_mm_set1_epi32(0x7fffffff));

	const XmmLink& tmp0 = XmmAlloc();
	const XmmLink& tmp1 = XmmAlloc();
	const XmmLink& tmp2 = XmmAlloc();
	const XmmLink& tmp3 = XmmAlloc();
	const XmmLink& tmpv = XmmAlloc();

	c->pxor(tmp0, tmp0);
	c->pxor(tmp1, tmp1);
	c->cmpps(tmp0, SPU_OFF_128(gpr, op.ra), 3);  //tmp0 is true if a is extended (nan/inf)
	c->cmpps(tmp1, SPU_OFF_128(gpr, op.rb), 3);  //tmp1 is true if b is extended (nan/inf)

	//flush to 0 if denormalized
	c->pxor(tmpv, tmpv);
	c->movaps(tmp2, SPU_OFF_128(gpr, op.ra));
	c->movaps(tmp3, SPU_OFF_128(gpr, op.rb));
	c->andps(tmp2, all_exp_bits);
	c->andps(tmp3, all_exp_bits);
	c->cmpps(tmp2, tmpv, 0);
	c->cmpps(tmp3, tmpv, 0);
	c->pandn(tmp2, SPU_OFF_128(gpr, op.ra));
	c->pandn(tmp3, SPU_OFF_128(gpr, op.rb));

	//Set tmp1 to true where a is extended but b is not extended
	//This is a simplification since absolute values remove necessity of lowering
	c->xorps(tmp0, tmp1);   //tmp0 is true when either a or b is extended
	c->pandn(tmp1, tmp0);   //tmp1 is true if b is not extended and a is extended

	c->andps(tmp2, remove_sign_bits);
	c->andps(tmp3, remove_sign_bits);
	c->cmpps(tmp3, tmp2, 1);
	c->orps(tmp3, tmp1);    //Force result to all true if a is extended but b is not
	c->movaps(SPU_OFF_128(gpr, op.rt), tmp3);
}

void spu_recompiler::DFCMGT(spu_opcode_t op)
{
	const auto mask = XmmConst(_mm_set1_epi64x(0x7fffffffffffffff));
	const XmmLink& va = XmmGet(op.ra, XmmType::Double);
	const XmmLink& vb = XmmGet(op.rb, XmmType::Double);

	c->andpd(va, mask);
	c->andpd(vb, mask);
	c->cmppd(vb, va, 1);
	c->movaps(SPU_OFF_128(gpr, op.rt), vb);
}

void spu_recompiler::DFA(spu_opcode_t op)
{
	const XmmLink& va = XmmGet(op.ra, XmmType::Double);
	c->addpd(va, SPU_OFF_128(gpr, op.rb));
	c->movapd(SPU_OFF_128(gpr, op.rt), va);
}

void spu_recompiler::DFS(spu_opcode_t op)
{
	const XmmLink& va = XmmGet(op.ra, XmmType::Double);
	c->subpd(va, SPU_OFF_128(gpr, op.rb));
	c->movapd(SPU_OFF_128(gpr, op.rt), va);
}

void spu_recompiler::DFM(spu_opcode_t op)
{
	const XmmLink& va = XmmGet(op.ra, XmmType::Double);
	c->mulpd(va, SPU_OFF_128(gpr, op.rb));
	c->movapd(SPU_OFF_128(gpr, op.rt), va);
}

void spu_recompiler::CLGTB(spu_opcode_t op)
{
	// compare if-greater-than
	const XmmLink& va = XmmGet(op.ra, XmmType::Int);
	const XmmLink& vi = XmmAlloc();
	c->movdqa(vi, XmmConst(_mm_set1_epi8(INT8_MIN)));
	c->pxor(va, vi);
	c->pxor(vi, SPU_OFF_128(gpr, op.rb));
	c->pcmpgtb(va, vi);
	c->movdqa(SPU_OFF_128(gpr, op.rt), va);
}

void spu_recompiler::HLGT(spu_opcode_t op)
{
	c->mov(*addr, SPU_OFF_32(gpr, op.ra, &v128::_u32, 3));
	c->cmp(*addr, SPU_OFF_32(gpr, op.rb, &v128::_u32, 3));
	c->ja(halt(m_pos));
}

void spu_recompiler::DFMA(spu_opcode_t op)
{
	const XmmLink& vr = XmmGet(op.rt, XmmType::Double);
	const XmmLink& va = XmmGet(op.ra, XmmType::Double);
	c->mulpd(va, SPU_OFF_128(gpr, op.rb));
	c->addpd(vr, va);
	c->movapd(SPU_OFF_128(gpr, op.rt), vr);
}

void spu_recompiler::DFMS(spu_opcode_t op)
{
	const XmmLink& va = XmmGet(op.ra, XmmType::Double);
	const XmmLink& vt = XmmGet(op.rt, XmmType::Double);
	c->mulpd(va, SPU_OFF_128(gpr, op.rb));
	c->subpd(va, vt);
	c->movapd(SPU_OFF_128(gpr, op.rt), va);
}

void spu_recompiler::DFNMS(spu_opcode_t op)
{
	const XmmLink& vr = XmmGet(op.rt, XmmType::Double);
	const XmmLink& va = XmmGet(op.ra, XmmType::Double);
	c->mulpd(va, SPU_OFF_128(gpr, op.rb));
	c->subpd(vr, va);
	c->movapd(SPU_OFF_128(gpr, op.rt), vr);
}

void spu_recompiler::DFNMA(spu_opcode_t op)
{
	const XmmLink& va = XmmGet(op.ra, XmmType::Double);
	const XmmLink& vt = XmmGet(op.rt, XmmType::Double);
	c->mulpd(va, SPU_OFF_128(gpr, op.rb));
	c->addpd(vt, va);
	c->xorpd(va, va);
	c->subpd(va, vt);
	c->movapd(SPU_OFF_128(gpr, op.rt), va);
}

void spu_recompiler::CEQ(spu_opcode_t op)
{
	const XmmLink& va = XmmGet(op.ra, XmmType::Int);
	c->pcmpeqd(va, SPU_OFF_128(gpr, op.rb));
	c->movdqa(SPU_OFF_128(gpr, op.rt), va);
}

void spu_recompiler::MPYHHU(spu_opcode_t op)
{
	const XmmLink& va = XmmGet(op.ra, XmmType::Int);
	const XmmLink& vb = XmmGet(op.rb, XmmType::Int);
	const XmmLink& va2 = XmmAlloc();
	c->movdqa(va2, va);
	c->pmulhuw(va, vb);
	c->pmullw(va2, vb);
	c->pand(va, XmmConst(_mm_set1_epi32(0xffff0000)));
	c->psrld(va2, 16);
	c->por(va, va2);
	c->movdqa(SPU_OFF_128(gpr, op.rt), va);
}

void spu_recompiler::ADDX(spu_opcode_t op)
{
	const XmmLink& vt = XmmGet(op.rt, XmmType::Int);
	c->pand(vt, XmmConst(_mm_set1_epi32(1)));
	c->paddd(vt, SPU_OFF_128(gpr, op.ra));
	c->paddd(vt, SPU_OFF_128(gpr, op.rb));
	c->movdqa(SPU_OFF_128(gpr, op.rt), vt);
}

void spu_recompiler::SFX(spu_opcode_t op)
{
	const XmmLink& vt = XmmGet(op.rt, XmmType::Int);
	const XmmLink& vb = XmmGet(op.rb, XmmType::Int);
	c->pandn(vt, XmmConst(_mm_set1_epi32(1)));
	c->psubd(vb, SPU_OFF_128(gpr, op.ra));
	c->psubd(vb, vt);
	c->movdqa(SPU_OFF_128(gpr, op.rt), vb);
}

void spu_recompiler::CGX(spu_opcode_t op) //nf
{
	for (u32 i = 0; i < 4; i++) // unrolled loop
	{
		c->bt(SPU_OFF_32(gpr, op.rt, &v128::_u32, i), 0);
		c->mov(*addr, SPU_OFF_32(gpr, op.ra, &v128::_u32, i));
		c->adc(*addr, SPU_OFF_32(gpr, op.rb, &v128::_u32, i));
		c->setc(addr->r8());
		c->movzx(*addr, addr->r8());
		c->mov(SPU_OFF_32(gpr, op.rt, &v128::_u32, i), *addr);
	}
}

void spu_recompiler::BGX(spu_opcode_t op) //nf
{
	for (u32 i = 0; i < 4; i++) // unrolled loop
	{
		c->bt(SPU_OFF_32(gpr, op.rt, &v128::_u32, i), 0);
		c->cmc();
		c->mov(*addr, SPU_OFF_32(gpr, op.rb, &v128::_u32, i));
		c->sbb(*addr, SPU_OFF_32(gpr, op.ra, &v128::_u32, i));
		c->setnc(addr->r8());
		c->movzx(*addr, addr->r8());
		c->mov(SPU_OFF_32(gpr, op.rt, &v128::_u32, i), *addr);
	}
}

void spu_recompiler::MPYHHA(spu_opcode_t op)
{
	const XmmLink& vt = XmmGet(op.rt, XmmType::Int);
	const XmmLink& va = XmmGet(op.ra, XmmType::Int);
	const XmmLink& vb = XmmGet(op.rb, XmmType::Int);
	c->psrld(va, 16);
	c->psrld(vb, 16);
	c->pmaddwd(va, vb);
	c->paddd(vt, va);
	c->movdqa(SPU_OFF_128(gpr, op.rt), vt);
}

void spu_recompiler::MPYHHAU(spu_opcode_t op)
{
	const XmmLink& vt = XmmGet(op.rt, XmmType::Int);
	const XmmLink& va = XmmGet(op.ra, XmmType::Int);
	const XmmLink& vb = XmmGet(op.rb, XmmType::Int);
	const XmmLink& va2 = XmmAlloc();
	c->movdqa(va2, va);
	c->pmulhuw(va, vb);
	c->pmullw(va2, vb);
	c->pand(va, XmmConst(_mm_set1_epi32(0xffff0000)));
	c->psrld(va2, 16);
	c->paddd(vt, va);
	c->paddd(vt, va2);
	c->movdqa(SPU_OFF_128(gpr, op.rt), vt);
}

void spu_recompiler::FSCRRD(spu_opcode_t op)
{
	// zero (hack)
	const XmmLink& v0 = XmmAlloc();
	c->pxor(v0, v0);
	c->movdqa(SPU_OFF_128(gpr, op.rt), v0);
}

void spu_recompiler::FESD(spu_opcode_t op)
{
	const XmmLink& va = XmmGet(op.ra, XmmType::Float);
	c->shufps(va, va, 0x8d); // _f[0] = _f[1]; _f[1] = _f[3];
	c->cvtps2pd(va, va);
	c->movapd(SPU_OFF_128(gpr, op.rt), va);
}

void spu_recompiler::FRDS(spu_opcode_t op)
{
	const XmmLink& va = XmmGet(op.ra, XmmType::Double);
	c->cvtpd2ps(va, va);
	c->shufps(va, va, 0x72); // _f[1] = _f[0]; _f[3] = _f[1]; _f[0] = _f[2] = 0;
	c->movaps(SPU_OFF_128(gpr, op.rt), va);
}

void spu_recompiler::FSCRWR(spu_opcode_t op)
{
	// nop (not implemented)
}

void spu_recompiler::DFTSV(spu_opcode_t op)
{
	fmt::throw_exception("Unexpected instruction" HERE);
}

void spu_recompiler::FCEQ(spu_opcode_t op)
{
	// compare equal
	const XmmLink& vb = XmmGet(op.rb, XmmType::Float);
	c->cmpps(vb, SPU_OFF_128(gpr, op.ra), 0);
	c->movaps(SPU_OFF_128(gpr, op.rt), vb);
}

void spu_recompiler::DFCEQ(spu_opcode_t op)
{
	fmt::throw_exception("Unexpected instruction" HERE);
}

void spu_recompiler::MPY(spu_opcode_t op)
{
	const XmmLink& va = XmmGet(op.ra, XmmType::Int);
	const XmmLink& vb = XmmGet(op.rb, XmmType::Int);
	const XmmLink& vi = XmmAlloc();
	c->movdqa(vi, XmmConst(_mm_set1_epi32(0xffff)));
	c->pand(va, vi);
	c->pand(vb, vi);
	c->pmaddwd(va, vb);
	c->movdqa(SPU_OFF_128(gpr, op.rt), va);
}

void spu_recompiler::MPYH(spu_opcode_t op)
{
	const XmmLink& va = XmmGet(op.ra, XmmType::Int);
	const XmmLink& vb = XmmGet(op.rb, XmmType::Int);
	c->psrld(va, 16);
	c->pmullw(va, vb);
	c->pslld(va, 16);
	c->movdqa(SPU_OFF_128(gpr, op.rt), va);
}

void spu_recompiler::MPYHH(spu_opcode_t op)
{
	const XmmLink& va = XmmGet(op.ra, XmmType::Int);
	const XmmLink& vb = XmmGet(op.rb, XmmType::Int);
	c->psrld(va, 16);
	c->psrld(vb, 16);
	c->pmaddwd(va, vb);
	c->movdqa(SPU_OFF_128(gpr, op.rt), va);
}

void spu_recompiler::MPYS(spu_opcode_t op)
{
	const XmmLink& va = XmmGet(op.ra, XmmType::Int);
	const XmmLink& vb = XmmGet(op.rb, XmmType::Int);
	c->pmulhw(va, vb);
	c->pslld(va, 16);
	c->psrad(va, 16);
	c->movdqa(SPU_OFF_128(gpr, op.rt), va);
}

void spu_recompiler::CEQH(spu_opcode_t op)
{
	const XmmLink& va = XmmGet(op.ra, XmmType::Int);
	c->pcmpeqw(va, SPU_OFF_128(gpr, op.rb));
	c->movdqa(SPU_OFF_128(gpr, op.rt), va);
}

void spu_recompiler::FCMEQ(spu_opcode_t op)
{
	const XmmLink& vb = XmmGet(op.rb, XmmType::Float);
	const XmmLink& vi = XmmAlloc();
	c->movaps(vi, XmmConst(_mm_set1_epi32(0x7fffffff)));
	c->andps(vb, vi); // abs
	c->andps(vi, SPU_OFF_128(gpr, op.ra));
	c->cmpps(vb, vi, 0); // ==
	c->movaps(SPU_OFF_128(gpr, op.rt), vb);
}

void spu_recompiler::DFCMEQ(spu_opcode_t op)
{
	fmt::throw_exception("Unexpected instruction" HERE);
}

void spu_recompiler::MPYU(spu_opcode_t op)
{
	const XmmLink& va = XmmGet(op.ra, XmmType::Int);
	const XmmLink& vb = XmmGet(op.rb, XmmType::Int);
	const XmmLink& va2 = XmmAlloc();
	c->movdqa(va2, va);
	c->pmulhuw(va, vb);
	c->pmullw(va2, vb);
	c->pslld(va, 16);
	c->pand(va2, XmmConst(_mm_set1_epi32(0xffff)));
	c->por(va, va2);
	c->movdqa(SPU_OFF_128(gpr, op.rt), va);
}

void spu_recompiler::CEQB(spu_opcode_t op)
{
	const XmmLink& va = XmmGet(op.ra, XmmType::Int);
	c->pcmpeqb(va, SPU_OFF_128(gpr, op.rb));
	c->movdqa(SPU_OFF_128(gpr, op.rt), va);
}

void spu_recompiler::FI(spu_opcode_t op)
{
	// Floating Interpolate
	const XmmLink& vb = XmmGet(op.rb, XmmType::Float);
	c->movaps(SPU_OFF_128(gpr, op.rt), vb);
}

void spu_recompiler::HEQ(spu_opcode_t op)
{
	c->mov(*addr, SPU_OFF_32(gpr, op.ra, &v128::_s32, 3));
	c->cmp(*addr, SPU_OFF_32(gpr, op.rb, &v128::_s32, 3));
	c->je(halt(m_pos));
}

void spu_recompiler::CFLTS(spu_opcode_t op)
{
	const XmmLink& va = XmmGet(op.ra, XmmType::Float);
	const XmmLink& vi = XmmAlloc();
	if (op.i8 != 173) c->mulps(va, XmmConst(_mm_set1_ps(std::exp2(static_cast<float>(static_cast<s16>(173 - op.i8)))))); // scale
	c->movaps(vi, XmmConst(_mm_set1_ps(std::exp2(31.f))));
	c->cmpps(vi, va, 2);
	c->cvttps2dq(va, va); // convert to ints with truncation
	c->pxor(va, vi); // fix result saturation (0x80000000 -> 0x7fffffff)
	c->movdqa(SPU_OFF_128(gpr, op.rt), va);
}

void spu_recompiler::CFLTU(spu_opcode_t op)
{
	const XmmLink& va = XmmGet(op.ra, XmmType::Float);
	const XmmLink& vs = XmmAlloc();
	const XmmLink& vs2 = XmmAlloc();
	const XmmLink& vs3 = XmmAlloc();
	if (op.i8 != 173) c->mulps(va, XmmConst(_mm_set1_ps(std::exp2(static_cast<float>(static_cast<s16>(173 - op.i8)))))); // scale

	if (utils::has_512())
	{
		c->vcvttps2udq(vs, va);
		c->psrad(va, 31);
		c->pandn(va, vs);
		c->movdqa(SPU_OFF_128(gpr, op.rt), va);
		return;
	}

	c->movdqa(vs, va);
	c->psrad(va, 31);
	c->andnps(va, vs);
	c->movaps(vs, va); // copy scaled value
	c->movaps(vs2, va);
	c->movaps(vs3, XmmConst(_mm_set1_ps(std::exp2(31.f))));
	c->subps(vs2, vs3);
	c->cmpps(vs3, vs, 2);
	c->andps(vs2, vs3);
	c->cvttps2dq(va, va);
	c->cmpps(vs, XmmConst(_mm_set1_ps(std::exp2(32.f))), 5);
	c->cvttps2dq(vs2, vs2);
	c->por(va, vs);
	c->por(va, vs2);
	c->movdqa(SPU_OFF_128(gpr, op.rt), va);
}

void spu_recompiler::CSFLT(spu_opcode_t op)
{
	const XmmLink& va = XmmGet(op.ra, XmmType::Int);
	c->cvtdq2ps(va, va); // convert to floats
	if (op.i8 != 155) c->mulps(va, XmmConst(_mm_set1_ps(std::exp2(static_cast<float>(static_cast<s16>(op.i8 - 155)))))); // scale
	c->movaps(SPU_OFF_128(gpr, op.rt), va);
}

void spu_recompiler::CUFLT(spu_opcode_t op)
{
	const XmmLink& va = XmmGet(op.ra, XmmType::Int);
	const XmmLink& v1 = XmmAlloc();

	if (utils::has_512())
	{
		c->vcvtudq2ps(va, va);
	}
	else
	{
		c->movdqa(v1, va);
		c->pand(va, XmmConst(_mm_set1_epi32(0x7fffffff)));
		c->cvtdq2ps(va, va); // convert to floats
		c->psrad(v1, 31); // generate mask from sign bit
		c->andps(v1, XmmConst(_mm_set1_ps(std::exp2(31.f)))); // generate correction component
		c->addps(va, v1); // add correction component
	}

	if (op.i8 != 155) c->mulps(va, XmmConst(_mm_set1_ps(std::exp2(static_cast<float>(static_cast<s16>(op.i8 - 155)))))); // scale
	c->movaps(SPU_OFF_128(gpr, op.rt), va);
}

void spu_recompiler::BRZ(spu_opcode_t op)
{
	const u32 target = spu_branch_target(m_pos, op.i16);

	if (target == m_pos + 4)
	{
		return;
	}

	asmjit::Label branch_label = c->newLabel();
	c->cmp(SPU_OFF_32(gpr, op.rt, &v128::_u32, 3), 0);
	c->je(branch_label);

	after.emplace_back([=]
	{
		c->align(asmjit::kAlignCode, 16);
		c->bind(branch_label);
		branch_fixed(target);
	});
}

void spu_recompiler::STQA(spu_opcode_t op)
{
	if (utils::has_ssse3())
	{
		const XmmLink& vt = XmmGet(op.rt, XmmType::Int);
		c->pshufb(vt, XmmConst(_mm_set_epi32(0x00010203, 0x04050607, 0x08090a0b, 0x0c0d0e0f)));
		c->movdqa(asmjit::x86::oword_ptr(*ls, spu_ls_target(0, op.i16)), vt);
	}
	else
	{
		c->mov(*qw0, SPU_OFF_64(gpr, op.rt, &v128::_u64, 0));
		c->mov(*qw1, SPU_OFF_64(gpr, op.rt, &v128::_u64, 1));
		c->bswap(*qw0);
		c->bswap(*qw1);
		c->mov(asmjit::x86::qword_ptr(*ls, spu_ls_target(0, op.i16) + 0), *qw1);
		c->mov(asmjit::x86::qword_ptr(*ls, spu_ls_target(0, op.i16) + 8), *qw0);
	}
}

void spu_recompiler::BRNZ(spu_opcode_t op)
{
	const u32 target = spu_branch_target(m_pos, op.i16);

	if (target == m_pos + 4)
	{
		return;
	}

	asmjit::Label branch_label = c->newLabel();
	c->cmp(SPU_OFF_32(gpr, op.rt, &v128::_u32, 3), 0);
	c->jne(branch_label);

	after.emplace_back([=]
	{
		c->align(asmjit::kAlignCode, 16);
		c->bind(branch_label);
		branch_fixed(target);
	});
}

void spu_recompiler::BRHZ(spu_opcode_t op)
{
	const u32 target = spu_branch_target(m_pos, op.i16);

	if (target == m_pos + 4)
	{
		return;
	}

	asmjit::Label branch_label = c->newLabel();
	c->cmp(SPU_OFF_16(gpr, op.rt, &v128::_u16, 6), 0);
	c->je(branch_label);

	after.emplace_back([=]
	{
		c->align(asmjit::kAlignCode, 16);
		c->bind(branch_label);
		branch_fixed(target);
	});
}

void spu_recompiler::BRHNZ(spu_opcode_t op)
{
	const u32 target = spu_branch_target(m_pos, op.i16);

	if (target == m_pos + 4)
	{
		return;
	}

	asmjit::Label branch_label = c->newLabel();
	c->cmp(SPU_OFF_16(gpr, op.rt, &v128::_u16, 6), 0);
	c->jne(branch_label);

	after.emplace_back([=]
	{
		c->align(asmjit::kAlignCode, 16);
		c->bind(branch_label);
		branch_fixed(target);
	});
}

void spu_recompiler::STQR(spu_opcode_t op)
{
	if (utils::has_ssse3())
	{
		const XmmLink& vt = XmmGet(op.rt, XmmType::Int);
		c->pshufb(vt, XmmConst(_mm_set_epi32(0x00010203, 0x04050607, 0x08090a0b, 0x0c0d0e0f)));
		c->movdqa(asmjit::x86::oword_ptr(*ls, spu_ls_target(m_pos, op.i16)), vt);
	}
	else
	{
		c->mov(*qw0, SPU_OFF_64(gpr, op.rt, &v128::_u64, 0));
		c->mov(*qw1, SPU_OFF_64(gpr, op.rt, &v128::_u64, 1));
		c->bswap(*qw0);
		c->bswap(*qw1);
		c->mov(asmjit::x86::qword_ptr(*ls, spu_ls_target(m_pos, op.i16) + 0), *qw1);
		c->mov(asmjit::x86::qword_ptr(*ls, spu_ls_target(m_pos, op.i16) + 8), *qw0);
	}
}

void spu_recompiler::BRA(spu_opcode_t op)
{
	const u32 target = spu_branch_target(0, op.i16);

	if (target != m_pos + 4)
	{
		branch_fixed(target);
		m_pos = -1;
	}
}

void spu_recompiler::LQA(spu_opcode_t op)
{
	if (utils::has_ssse3())
	{
		const XmmLink& vt = XmmAlloc();
		c->movdqa(vt, asmjit::x86::oword_ptr(*ls, spu_ls_target(0, op.i16)));
		c->pshufb(vt, XmmConst(_mm_set_epi32(0x00010203, 0x04050607, 0x08090a0b, 0x0c0d0e0f)));
		c->movdqa(SPU_OFF_128(gpr, op.rt), vt);
	}
	else
	{
		c->mov(*qw0, asmjit::x86::qword_ptr(*ls, spu_ls_target(0, op.i16) + 0));
		c->mov(*qw1, asmjit::x86::qword_ptr(*ls, spu_ls_target(0, op.i16) + 8));
		c->bswap(*qw0);
		c->bswap(*qw1);
		c->mov(SPU_OFF_64(gpr, op.rt, &v128::_u64, 0), *qw1);
		c->mov(SPU_OFF_64(gpr, op.rt, &v128::_u64, 1), *qw0);
	}
}

void spu_recompiler::BRASL(spu_opcode_t op)
{
	const u32 target = spu_branch_target(0, op.i16);

	const XmmLink& vr = XmmAlloc();
	c->movdqa(vr, XmmConst(_mm_set_epi32(spu_branch_target(m_pos + 4), 0, 0, 0)));
	c->movdqa(SPU_OFF_128(gpr, op.rt), vr);

	if (target != m_pos + 4)
	{
		branch_fixed(target);
		m_pos = -1;
	}
}

void spu_recompiler::BR(spu_opcode_t op)
{
	const u32 target = spu_branch_target(m_pos, op.i16);

	if (target != m_pos + 4)
	{
		branch_fixed(target);
		m_pos = -1;
	}
}

void spu_recompiler::FSMBI(spu_opcode_t op)
{
	const XmmLink& vr = XmmAlloc();
	c->movdqa(vr, XmmConst(g_spu_imm.fsmb[op.i16]));
	c->movdqa(SPU_OFF_128(gpr, op.rt), vr);
}

void spu_recompiler::BRSL(spu_opcode_t op)
{
	const u32 target = spu_branch_target(m_pos, op.i16);

	const XmmLink& vr = XmmAlloc();
	c->movdqa(vr, XmmConst(_mm_set_epi32(spu_branch_target(m_pos + 4), 0, 0, 0)));
	c->movdqa(SPU_OFF_128(gpr, op.rt), vr);

	if (target != m_pos + 4)
	{
		branch_fixed(target);
		m_pos = -1;
	}
}

void spu_recompiler::LQR(spu_opcode_t op)
{
	if (utils::has_ssse3())
	{
		const XmmLink& vt = XmmAlloc();
		c->movdqa(vt, asmjit::x86::oword_ptr(*ls, spu_ls_target(m_pos, op.i16)));
		c->pshufb(vt, XmmConst(_mm_set_epi32(0x00010203, 0x04050607, 0x08090a0b, 0x0c0d0e0f)));
		c->movdqa(SPU_OFF_128(gpr, op.rt), vt);
	}
	else
	{
		c->mov(*qw0, asmjit::x86::qword_ptr(*ls, spu_ls_target(m_pos, op.i16) + 0));
		c->mov(*qw1, asmjit::x86::qword_ptr(*ls, spu_ls_target(m_pos, op.i16) + 8));
		c->bswap(*qw0);
		c->bswap(*qw1);
		c->mov(SPU_OFF_64(gpr, op.rt, &v128::_u64, 0), *qw1);
		c->mov(SPU_OFF_64(gpr, op.rt, &v128::_u64, 1), *qw0);
	}
}

void spu_recompiler::IL(spu_opcode_t op)
{
	const XmmLink& vr = XmmAlloc();
	c->movdqa(vr, XmmConst(_mm_set1_epi32(op.si16)));
	c->movdqa(SPU_OFF_128(gpr, op.rt), vr);
}

void spu_recompiler::ILHU(spu_opcode_t op)
{
	const XmmLink& vr = XmmAlloc();
	c->movdqa(vr, XmmConst(_mm_set1_epi32(op.i16 << 16)));
	c->movdqa(SPU_OFF_128(gpr, op.rt), vr);
}

void spu_recompiler::ILH(spu_opcode_t op)
{
	const XmmLink& vr = XmmAlloc();
	c->movdqa(vr, XmmConst(_mm_set1_epi16(op.i16)));
	c->movdqa(SPU_OFF_128(gpr, op.rt), vr);
}

void spu_recompiler::IOHL(spu_opcode_t op)
{
	const XmmLink& vt = XmmGet(op.rt, XmmType::Int);
	c->por(vt, XmmConst(_mm_set1_epi32(op.i16)));
	c->movdqa(SPU_OFF_128(gpr, op.rt), vt);
}

void spu_recompiler::ORI(spu_opcode_t op)
{
	const XmmLink& va = XmmGet(op.ra, XmmType::Int);
	if (op.si10) c->por(va, XmmConst(_mm_set1_epi32(op.si10)));
	c->movdqa(SPU_OFF_128(gpr, op.rt), va);
}

void spu_recompiler::ORHI(spu_opcode_t op)
{
	const XmmLink& va = XmmGet(op.ra, XmmType::Int);
	c->por(va, XmmConst(_mm_set1_epi16(op.si10)));
	c->movdqa(SPU_OFF_128(gpr, op.rt), va);
}

void spu_recompiler::ORBI(spu_opcode_t op)
{
	const XmmLink& va = XmmGet(op.ra, XmmType::Int);
	c->por(va, XmmConst(_mm_set1_epi8(op.si10)));
	c->movdqa(SPU_OFF_128(gpr, op.rt), va);
}

void spu_recompiler::SFI(spu_opcode_t op)
{
	const XmmLink& vr = XmmAlloc();
	c->movdqa(vr, XmmConst(_mm_set1_epi32(op.si10)));
	c->psubd(vr, SPU_OFF_128(gpr, op.ra));
	c->movdqa(SPU_OFF_128(gpr, op.rt), vr);
}

void spu_recompiler::SFHI(spu_opcode_t op)
{
	const XmmLink& vr = XmmAlloc();
	c->movdqa(vr, XmmConst(_mm_set1_epi16(op.si10)));
	c->psubw(vr, SPU_OFF_128(gpr, op.ra));
	c->movdqa(SPU_OFF_128(gpr, op.rt), vr);
}

void spu_recompiler::ANDI(spu_opcode_t op)
{
	const XmmLink& va = XmmGet(op.ra, XmmType::Int);
	c->pand(va, XmmConst(_mm_set1_epi32(op.si10)));
	c->movdqa(SPU_OFF_128(gpr, op.rt), va);
}

void spu_recompiler::ANDHI(spu_opcode_t op)
{
	const XmmLink& va = XmmGet(op.ra, XmmType::Int);
	c->pand(va, XmmConst(_mm_set1_epi16(op.si10)));
	c->movdqa(SPU_OFF_128(gpr, op.rt), va);
}

void spu_recompiler::ANDBI(spu_opcode_t op)
{
	const XmmLink& va = XmmGet(op.ra, XmmType::Int);
	c->pand(va, XmmConst(_mm_set1_epi8(op.si10)));
	c->movdqa(SPU_OFF_128(gpr, op.rt), va);
}

void spu_recompiler::AI(spu_opcode_t op)
{
	// add
	const XmmLink& va = XmmGet(op.ra, XmmType::Int);
	c->paddd(va, XmmConst(_mm_set1_epi32(op.si10)));
	c->movdqa(SPU_OFF_128(gpr, op.rt), va);
}

void spu_recompiler::AHI(spu_opcode_t op)
{
	// add
	const XmmLink& va = XmmGet(op.ra, XmmType::Int);
	c->paddw(va, XmmConst(_mm_set1_epi16(op.si10)));
	c->movdqa(SPU_OFF_128(gpr, op.rt), va);
}

void spu_recompiler::STQD(spu_opcode_t op)
{
	c->mov(*addr, SPU_OFF_32(gpr, op.ra, &v128::_u32, 3));
	if (op.si10) c->add(*addr, op.si10 << 4);
	c->and_(*addr, 0x3fff0);

	if (utils::has_ssse3())
	{
		const XmmLink& vt = XmmGet(op.rt, XmmType::Int);
		c->pshufb(vt, XmmConst(_mm_set_epi32(0x00010203, 0x04050607, 0x08090a0b, 0x0c0d0e0f)));
		c->movdqa(asmjit::x86::oword_ptr(*ls, addr->r64()), vt);
	}
	else
	{
		c->mov(*qw0, SPU_OFF_64(gpr, op.rt, &v128::_u64, 0));
		c->mov(*qw1, SPU_OFF_64(gpr, op.rt, &v128::_u64, 1));
		c->bswap(*qw0);
		c->bswap(*qw1);
		c->mov(asmjit::x86::qword_ptr(*ls, addr->r64(), 0, 0), *qw1);
		c->mov(asmjit::x86::qword_ptr(*ls, addr->r64(), 0, 8), *qw0);
	}
}

void spu_recompiler::LQD(spu_opcode_t op)
{
	c->mov(*addr, SPU_OFF_32(gpr, op.ra, &v128::_u32, 3));
	if (op.si10) c->add(*addr, op.si10 << 4);
	c->and_(*addr, 0x3fff0);

	if (utils::has_ssse3())
	{
		const XmmLink& vt = XmmAlloc();
		c->movdqa(vt, asmjit::x86::oword_ptr(*ls, addr->r64()));
		c->pshufb(vt, XmmConst(_mm_set_epi32(0x00010203, 0x04050607, 0x08090a0b, 0x0c0d0e0f)));
		c->movdqa(SPU_OFF_128(gpr, op.rt), vt);
	}
	else
	{
		c->mov(*qw0, asmjit::x86::qword_ptr(*ls, addr->r64(), 0, 0));
		c->mov(*qw1, asmjit::x86::qword_ptr(*ls, addr->r64(), 0, 8));
		c->bswap(*qw0);
		c->bswap(*qw1);
		c->mov(SPU_OFF_64(gpr, op.rt, &v128::_u64, 0), *qw1);
		c->mov(SPU_OFF_64(gpr, op.rt, &v128::_u64, 1), *qw0);
	}
}

void spu_recompiler::XORI(spu_opcode_t op)
{
	const XmmLink& va = XmmGet(op.ra, XmmType::Int);
	c->pxor(va, XmmConst(_mm_set1_epi32(op.si10)));
	c->movdqa(SPU_OFF_128(gpr, op.rt), va);
}

void spu_recompiler::XORHI(spu_opcode_t op)
{
	const XmmLink& va = XmmGet(op.ra, XmmType::Int);
	c->pxor(va, XmmConst(_mm_set1_epi16(op.si10)));
	c->movdqa(SPU_OFF_128(gpr, op.rt), va);
}

void spu_recompiler::XORBI(spu_opcode_t op)
{
	const XmmLink& va = XmmGet(op.ra, XmmType::Int);
	c->pxor(va, XmmConst(_mm_set1_epi8(op.si10)));
	c->movdqa(SPU_OFF_128(gpr, op.rt), va);
}

void spu_recompiler::CGTI(spu_opcode_t op)
{
	const XmmLink& va = XmmGet(op.ra, XmmType::Int);
	c->pcmpgtd(va, XmmConst(_mm_set1_epi32(op.si10)));
	c->movdqa(SPU_OFF_128(gpr, op.rt), va);
}

void spu_recompiler::CGTHI(spu_opcode_t op)
{
	const XmmLink& va = XmmGet(op.ra, XmmType::Int);
	c->pcmpgtw(va, XmmConst(_mm_set1_epi16(op.si10)));
	c->movdqa(SPU_OFF_128(gpr, op.rt), va);
}

void spu_recompiler::CGTBI(spu_opcode_t op)
{
	const XmmLink& va = XmmGet(op.ra, XmmType::Int);
	c->pcmpgtb(va, XmmConst(_mm_set1_epi8(op.si10)));
	c->movdqa(SPU_OFF_128(gpr, op.rt), va);
}

void spu_recompiler::HGTI(spu_opcode_t op)
{
	c->cmp(SPU_OFF_32(gpr, op.ra, &v128::_s32, 3), op.si10);
	c->jg(halt(m_pos));
}

void spu_recompiler::CLGTI(spu_opcode_t op)
{
	const XmmLink& va = XmmGet(op.ra, XmmType::Int);
	c->pxor(va, XmmConst(_mm_set1_epi32(0x80000000)));
	c->pcmpgtd(va, XmmConst(_mm_set1_epi32(op.si10 - 0x80000000)));
	c->movdqa(SPU_OFF_128(gpr, op.rt), va);
}

void spu_recompiler::CLGTHI(spu_opcode_t op)
{
	const XmmLink& va = XmmGet(op.ra, XmmType::Int);
	c->pxor(va, XmmConst(_mm_set1_epi16(INT16_MIN)));
	c->pcmpgtw(va, XmmConst(_mm_set1_epi16(op.si10 - 0x8000)));
	c->movdqa(SPU_OFF_128(gpr, op.rt), va);
}

void spu_recompiler::CLGTBI(spu_opcode_t op)
{
	const XmmLink& va = XmmGet(op.ra, XmmType::Int);
	c->psubb(va, XmmConst(_mm_set1_epi8(INT8_MIN)));
	c->pcmpgtb(va, XmmConst(_mm_set1_epi8(op.si10 - 0x80)));
	c->movdqa(SPU_OFF_128(gpr, op.rt), va);
}

void spu_recompiler::HLGTI(spu_opcode_t op)
{
	c->cmp(SPU_OFF_32(gpr, op.ra, &v128::_u32, 3), op.si10);
	c->ja(halt(m_pos));
}

void spu_recompiler::MPYI(spu_opcode_t op)
{
	const XmmLink& va = XmmGet(op.ra, XmmType::Int);
	c->pmaddwd(va, XmmConst(_mm_set1_epi32(op.si10 & 0xffff)));
	c->movdqa(SPU_OFF_128(gpr, op.rt), va);
}

void spu_recompiler::MPYUI(spu_opcode_t op)
{
	const XmmLink& va = XmmGet(op.ra, XmmType::Int);
	const XmmLink& vi = XmmAlloc();
	const XmmLink& va2 = XmmAlloc();
	c->movdqa(va2, va);
	c->movdqa(vi, XmmConst(_mm_set1_epi32(op.si10 & 0xffff)));
	c->pmulhuw(va, vi);
	c->pmullw(va2, vi);
	c->pslld(va, 16);
	c->por(va, va2);
	c->movdqa(SPU_OFF_128(gpr, op.rt), va);
}

void spu_recompiler::CEQI(spu_opcode_t op)
{
	const XmmLink& va = XmmGet(op.ra, XmmType::Int);
	c->pcmpeqd(va, XmmConst(_mm_set1_epi32(op.si10)));
	c->movdqa(SPU_OFF_128(gpr, op.rt), va);
}

void spu_recompiler::CEQHI(spu_opcode_t op)
{
	const XmmLink& va = XmmGet(op.ra, XmmType::Int);
	c->pcmpeqw(va, XmmConst(_mm_set1_epi16(op.si10)));
	c->movdqa(SPU_OFF_128(gpr, op.rt), va);
}

void spu_recompiler::CEQBI(spu_opcode_t op)
{
	const XmmLink& va = XmmGet(op.ra, XmmType::Int);
	c->pcmpeqb(va, XmmConst(_mm_set1_epi8(op.si10)));
	c->movdqa(SPU_OFF_128(gpr, op.rt), va);
}

void spu_recompiler::HEQI(spu_opcode_t op)
{
	c->cmp(SPU_OFF_32(gpr, op.ra, &v128::_u32, 3), op.si10);
	c->je(halt(m_pos));
}

void spu_recompiler::HBRA(spu_opcode_t op)
{
}

void spu_recompiler::HBRR(spu_opcode_t op)
{
}

void spu_recompiler::ILA(spu_opcode_t op)
{
	const XmmLink& vr = XmmAlloc();
	c->movdqa(vr, XmmConst(_mm_set1_epi32(op.i18)));
	c->movdqa(SPU_OFF_128(gpr, op.rt), vr);
}

void spu_recompiler::SELB(spu_opcode_t op)
{
	const XmmLink& vb = XmmGet(op.rb, XmmType::Int);
	const XmmLink& vc = XmmGet(op.rc, XmmType::Int);

	if (utils::has_512())
	{
		c->vpternlogd(vc, vb, SPU_OFF_128(gpr, op.ra), 0xca /* A?B:C */);
		c->movdqa(SPU_OFF_128(gpr, op.rt4), vc);
		return;
	}

	if (utils::has_xop())
	{
		c->vpcmov(vc, vb, SPU_OFF_128(gpr, op.ra), vc);
		c->movdqa(SPU_OFF_128(gpr, op.rt4), vc);
		return;
	}

	c->pand(vb, vc);
	c->pandn(vc, SPU_OFF_128(gpr, op.ra));
	c->por(vb, vc);
	c->movdqa(SPU_OFF_128(gpr, op.rt4), vb);
}

void spu_recompiler::SHUFB(spu_opcode_t op)
{
	if (0 && utils::has_512())
	{
		// Deactivated due to poor performance of mask merge ops.
		const XmmLink& va = XmmGet(op.ra, XmmType::Int);
		const XmmLink& vb = XmmGet(op.rb, XmmType::Int);
		const XmmLink& vc = XmmGet(op.rc, XmmType::Int);
		const XmmLink& vt = XmmAlloc();
		const XmmLink& vm = XmmAlloc();
		c->vpcmpub(asmjit::x86::k1, vc, XmmConst(_mm_set1_epi8(-0x40)), 5 /* GE */);
		c->vpxor(vm, vc, XmmConst(_mm_set1_epi8(0xf)));
		c->setExtraReg(asmjit::x86::k1);
		c->z().vblendmb(vc, vc, XmmConst(_mm_set1_epi8(-1))); // {k1}
		c->vpcmpub(asmjit::x86::k2, vm, XmmConst(_mm_set1_epi8(-0x20)), 5 /* GE */);
		c->vptestmb(asmjit::x86::k1, vm, XmmConst(_mm_set1_epi8(0x10)));
		c->vpshufb(vt, va, vm);
		c->setExtraReg(asmjit::x86::k2);
		c->z().vblendmb(va, va, XmmConst(_mm_set1_epi8(0x7f))); // {k2}
		c->setExtraReg(asmjit::x86::k1);
		c->vpshufb(vt, vb, vm); // {k1}
		c->vpternlogd(vt, va, vc, 0xf6 /* orAxorBC */);
		c->movdqa(SPU_OFF_128(gpr, op.rt4), vt);
		return;
	}

	if (!utils::has_ssse3())
	{
		return fall(op);
	}

	const XmmLink& va = XmmGet(op.ra, XmmType::Int);
	const XmmLink& vb = XmmGet(op.rb, XmmType::Int);
	const XmmLink& vc = XmmGet(op.rc, XmmType::Int);
	const XmmLink& vt = XmmAlloc();
	const XmmLink& vm = XmmAlloc();
	const XmmLink& v5 = XmmAlloc();
	c->movdqa(vm, XmmConst(_mm_set1_epi8(0xc0)));

	// Test for (110xxxxx) and (11xxxxxx) bit values
	if (utils::has_avx())
	{
		c->vpand(v5, vc, XmmConst(_mm_set1_epi8(0xe0)));
		c->vpand(vt, vc, vm);
	}
	else
	{
		c->movdqa(v5, vc);
		c->pand(v5, XmmConst(_mm_set1_epi8(0xe0)));
		c->movdqa(vt, vc);
		c->pand(vt, vm);
	}

	c->pxor(vc, XmmConst(_mm_set1_epi8(0xf)));
	c->pshufb(va, vc);
	c->pshufb(vb, vc);
	c->pand(vc, XmmConst(_mm_set1_epi8(0x10)));
	c->pcmpeqb(v5, vm); // If true, result should become 0xFF
	c->pcmpeqb(vt, vm); // If true, result should become either 0xFF or 0x80
	c->pavgb(vt, v5); // Generate result constant: AVG(0xff, 0x00) == 0x80
	c->pxor(vm, vm);
	c->pcmpeqb(vc, vm);

	// Select result value from va or vb
	if (utils::has_512())
	{
		c->vpternlogd(vc, va, vb, 0xca /* A?B:C */);
	}
	else if (utils::has_xop())
	{
		c->vpcmov(vc, va, vb, vc);
	}
	else
	{
		c->pand(va, vc);
		c->pandn(vc, vb);
		c->por(vc, va);
	}

	c->por(vt, vc);
	c->movdqa(SPU_OFF_128(gpr, op.rt4), vt);
}

void spu_recompiler::MPYA(spu_opcode_t op)
{
	const XmmLink& va = XmmGet(op.ra, XmmType::Int);
	const XmmLink& vb = XmmGet(op.rb, XmmType::Int);
	const XmmLink& vi = XmmAlloc();
	c->movdqa(vi, XmmConst(_mm_set1_epi32(0xffff)));
	c->pand(va, vi);
	c->pand(vb, vi);
	c->pmaddwd(va, vb);
	c->paddd(va, SPU_OFF_128(gpr, op.rc));
	c->movdqa(SPU_OFF_128(gpr, op.rt4), va);
}

void spu_recompiler::FNMS(spu_opcode_t op)
{
	const XmmLink& va = XmmGet(op.ra, XmmType::Float);
	const XmmLink& vb = XmmGet(op.rb, XmmType::Float);
	const XmmLink& mask = XmmAlloc();
	const XmmLink& v1 = XmmAlloc();
	const XmmLink& v2 = XmmAlloc();
	c->movaps(mask, XmmConst(_mm_set1_epi32(0x7f800000)));
	c->movaps(v1, va);
	c->movaps(v2, vb);
	c->andps(va, mask);
	c->andps(vb, mask);
	c->cmpps(va, mask, 4); // va = ra == extended
	c->cmpps(vb, mask, 4); // vb = rb == extended
	c->andps(va, v1); // va = ra & ~ra_extended
	c->andps(vb, v2); // vb = rb & ~rb_extended

	c->mulps(va, vb);
	c->movaps(vb, SPU_OFF_128(gpr, op.rc));
	c->subps(vb, va);
	c->movaps(SPU_OFF_128(gpr, op.rt4), vb);
}

void spu_recompiler::FMA(spu_opcode_t op)
{
	const XmmLink& va = XmmGet(op.ra, XmmType::Float);
	const XmmLink& vb = XmmGet(op.rb, XmmType::Float);
	const XmmLink& mask = XmmAlloc();
	const XmmLink& v1 = XmmAlloc();
	const XmmLink& v2 = XmmAlloc();
	c->movaps(mask, XmmConst(_mm_set1_epi32(0x7f800000)));
	c->movaps(v1, va);
	c->movaps(v2, vb);
	c->andps(va, mask);
	c->andps(vb, mask);
	c->cmpps(va, mask, 4); // va = ra == extended
	c->cmpps(vb, mask, 4); // vb = rb == extended
	c->andps(va, v1); // va = ra & ~ra_extended
	c->andps(vb, v2); // vb = rb & ~rb_extended

	c->mulps(va, vb);
	c->addps(va, SPU_OFF_128(gpr, op.rc));
	c->movaps(SPU_OFF_128(gpr, op.rt4), va);
}

void spu_recompiler::FMS(spu_opcode_t op)
{
	const XmmLink& va = XmmGet(op.ra, XmmType::Float);
	const XmmLink& vb = XmmGet(op.rb, XmmType::Float);
	const XmmLink& mask = XmmAlloc();
	const XmmLink& v1 = XmmAlloc();
	const XmmLink& v2 = XmmAlloc();
	c->movaps(mask, XmmConst(_mm_set1_epi32(0x7f800000)));
	c->movaps(v1, va);
	c->movaps(v2, vb);
	c->andps(va, mask);
	c->andps(vb, mask);
	c->cmpps(va, mask, 4); // va = ra == extended
	c->cmpps(vb, mask, 4); // vb = rb == extended
	c->andps(va, v1); // va = ra & ~ra_extended
	c->andps(vb, v2); // vb = rb & ~rb_extended

	c->mulps(va, vb);
	c->subps(va, SPU_OFF_128(gpr, op.rc));
	c->movaps(SPU_OFF_128(gpr, op.rt4), va);
}
