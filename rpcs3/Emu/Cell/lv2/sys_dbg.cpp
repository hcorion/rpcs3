#include "stdafx.h"
#include "Emu/Memory/Memory.h"
#include "Emu/System.h"
#include "Emu/IdManager.h"
#include "sys_spu.h"
#include "sys_event_flag.h"
#include "sys_event.h"
#include "sys_interrupt.h"

#include "Emu/Cell/SPUThread.h"
#include "Emu/Cell/RawSPUThread.h"

#include "Emu/Cell/ErrorCodes.h"
#include "sys_dbg.h"

extern u64 get_timebased_time();
namespace vm { using namespace ps3; }

logs::channel sys_dbg("sys_dbg");

void sys_dbg_get_console_type(vm::ptr<u64> Console_Type)
{
	*Console_Type = 1; // CEX console
}

error_code sys_dbg_read_spu_thread_context2(u32 id, vm::ptr<sys_dbg_spu_thread_context2_t> spu_context)
{
	const auto thread = idm::get<SPUThread>(id);

	if (!thread)
	{
		return CELL_ESRCH;
	}

	std::memcpy(spu_context.get_ptr() , std::addressof(thread->gpr) , sizeof(thread->gpr));
	(*spu_context).npc = thread->pc | (thread->events_state & 1);
	std::memcpy(&((*spu_context).fpscr) , std::addressof(thread->fpscr) , 16);
	(*spu_context).srr0 = thread->srr0;
	(*spu_context).spu_status = thread->status;
	(*spu_context).spu_cfg= thread->snr_config;
	(*spu_context).mb_stat= (thread->ch_out_mbox.get_count() & 0xff) | ((4 - thread->ch_in_mbox.get_count()) << 8 & 0xff00) | (thread->ch_out_intr_mbox.get_count() << 16 & 0xff0000);
	(*spu_context).ppu_mb = thread->ch_out_mbox.get_value();
	std::memcpy(((u32*)spu_context.get_ptr() + (sizeof(thread->gpr)/4) + 7) , std::addressof(thread->ch_in_mbox) + 2 , 12);
	*((u32*)spu_context.get_ptr() + (sizeof(thread->gpr)/4) + 10) = thread->ch_in_mbox.value3;
	(*spu_context).decrementer = thread->dec_state & 2 ? thread->ch_dec_value - (u32)(get_timebased_time() - thread->ch_dec_start_timestamp) : thread->ch_dec_value;

	return CELL_OK;
}
