#include "stdafx.h"
#include "Emu/Memory/Memory.h"
#include "Emu/System.h"

#include "Emu/Cell/ErrorCodes.h"
#include "sys_usbd.h"

namespace vm { using namespace ps3; }

logs::channel sys_storage("sys_storage");

s32 sys_storage_open(u64 device, u32 b, vm::ptr<u32> fd, u32 d)
{
	sys_storage.todo("sys_storage_open(device=0x%x, 0x%x, fd=*0x%x, 0x%x)", device, b, fd, d);
	*fd = 0xdadad0d0; // Poison value
	return CELL_OK;
}

s32 sys_storage_close(u32 fd)
{
	sys_storage.todo("sys_storage_close(fd=0x%x)", fd);
	return CELL_OK;
}

s32 sys_storage_read()
{
	sys_storage.todo("sys_storage_read()");
	return CELL_OK;
}

s32 sys_storage_write()
{
	sys_storage.todo("sys_storage_write()");
	return CELL_OK;
}

s32 sys_storage_send_device_command()
{
	sys_storage.todo("sys_storage_send_device_command()");
	return CELL_OK;
}

s32 sys_storage_async_configure()
{
	sys_storage.todo("sys_storage_async_configure()");
	return CELL_OK;
}

s32 sys_storage_async_read()
{
	sys_storage.todo("sys_storage_async_read()");
	return CELL_OK;
}

s32 sys_storage_async_write()
{
	sys_storage.todo("sys_storage_async_write()");
	return CELL_OK;
}

s32 sys_storage_async_cancel()
{
	sys_storage.todo("sys_storage_async_cancel()");
	return CELL_OK;
}

s32 sys_storage_get_device_info(u64 device, vm::ptr<u8> buffer)
{
	sys_storage.todo("sys_storage_get_device_info(device=0x%x, config=0x%x)", device, buffer);

	*reinterpret_cast<u64*>(buffer.get_ptr() + 0x28) = 1;
	buffer[0x3f] = 1;
	buffer[0x39] = 1;
	buffer[0x3a] = 1;
	return CELL_OK;
}

s32 sys_storage_get_device_config(u64 device, vm::ptr<u32> count)
{
	sys_storage.todo("sys_storage_get_device_config(device=*0x%x, count=0x%x)", device, count);


	*count = 1;

	return CELL_OK;
}

s32 sys_storage_report_devices(vm::ptr<void> a, u32 b, u32 count, vm::ptr<u64> device_ids)
{
	sys_storage.todo("sys_storage_report_devices(0x%x, 0x%x, count=0x%x, device_ids=0x%x)", a, b, count, device_ids);
	*device_ids = 0x101000000000006;
	return CELL_OK;
}

s32 sys_storage_configure_medium_event(u32 fd, u32 equeue_id, u32 c)
{
	sys_storage.todo("sys_storage_configure_medium_event(fd=0x%x, equeue_id=0x%x, 0x%x)", fd, equeue_id, c);
	return CELL_OK;
}

s32 sys_storage_set_medium_polling_interval()
{
	sys_storage.todo("sys_storage_set_medium_polling_interval()");
	return CELL_OK;
}

s32 sys_storage_create_region()
{
	sys_storage.todo("sys_storage_create_region()");
	return CELL_OK;
}

s32 sys_storage_delete_region()
{
	sys_storage.todo("sys_storage_delete_region()");
	return CELL_OK;
}

s32 sys_storage_execute_device_command(u32 fd, u32 b, vm::ptr<char> c, u32 d, vm::ptr<char> e, u32 f, vm::ptr<u32> driver_status)
{
	sys_storage.todo("sys_storage_execute_device_command(fd=0x%x, 0x%x, *0x%x, 0x%x, *0x%x, 0x%x, *0x%x)", fd, b, c, d, e, f, driver_status);
	return CELL_OK;
}

s32 sys_storage_check_region_acl()
{
	sys_storage.todo("sys_storage_check_region_acl()");
	return CELL_OK;
}

s32 sys_storage_set_region_acl()
{
	sys_storage.todo("sys_storage_set_region_acl()");
	return CELL_OK;
}

s32 sys_storage_async_send_device_command()
{
	sys_storage.todo("sys_storage_async_send_device_command()");
	return CELL_OK;
}

s32 sys_storage_get_region_offset()
{
	sys_storage.todo("sys_storage_get_region_offset()");
	return CELL_OK;
}

s32 sys_storage_set_emulated_speed()
{
	sys_storage.todo("sys_storage_set_emulated_speed()");
	return CELL_OK;
}
