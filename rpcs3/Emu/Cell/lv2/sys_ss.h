#pragma once

#include "Emu/Memory/Memory.h"
#include "Emu/Cell/ErrorCodes.h"

struct CellSsOpenPSID
{
	be_t<u64> high;
	be_t<u64> low;
};

error_code sys_ss_random_number_generator(u32 arg1, vm::ptr<void> buf, u64 size);
s32 sys_ss_get_console_id(vm::ptr<u8> buf);
s32 sys_ss_get_open_psid(vm::ptr<CellSsOpenPSID> ptr);

s32 sys_ss_363(u32 code, vm::ptr<u8> buffer);
s32 sys_ss_access_control_engine(u32 a, u32 b, u32 c);
s32 sys_ss_get_cache_of_product_mode(vm::ptr<u32/*vm::ptr<char>*/> ptr);
