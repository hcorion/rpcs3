#include "stdafx.h"
#include "sys_tty.h"

#include "sys_uart.h"
#include "sys_ppu_thread.h"

namespace vm { using namespace ps3; }

logs::channel sys_hid("sys_hid");

error_code sys_hid_510()
{
	sys_hid.todo("sys_hid_510()");
	return CELL_OK;
}

error_code sys_hid_514(u32 a, vm::ptr<u32> b, u32 c)
{
	sys_hid.todo("sys_hid_514(0x%x, 0x%x, 0x%x)", a, b, c);
	return CELL_OK;
}