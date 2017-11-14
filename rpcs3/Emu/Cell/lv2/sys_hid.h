#pragma once

#include "Emu/Memory/Memory.h"
#include "Emu/Cell/ErrorCodes.h"

// SysCalls
error_code sys_hid_510();
error_code sys_hid_514(u32 a, vm::ps3::ptr<u32> b, u32 c);