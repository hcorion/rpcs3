#pragma once

#include "Emu/Memory/Memory.h"
#include "Emu/Cell/ErrorCodes.h"

struct uart_packet
{
	u8 magic;
	u8 size;
	u8 cmd;
	u8 data[1];
};

// SysCalls
error_code sys_uart_initialize();
error_code sys_uart_receive(ppu_thread& ppu, vm::ps3::ptr<char> buffer, u64 size, u32 unk);
error_code sys_uart_send(ppu_thread& ppu, vm::ps3::cptr<u8> buffer, u64 size, u64 flags);
error_code sys_uart_get_params(vm::ps3::ptr<char> buffer);

error_code sys_sm_get_ext_event2(vm::ps3::ptr<u32> stack1, vm::ps3::ptr<u32> stack2, vm::ps3::ptr<u32> stack3, u32 maybe_bool);
error_code sys_sm_get_params(vm::ps3::ptr<u64> a, vm::ps3::ptr<u64> b, vm::ps3::ptr<u64> c, vm::ps3::ptr<u64> d);