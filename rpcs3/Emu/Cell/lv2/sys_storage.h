#pragma once

// SysCalls
s32 sys_storage_open(u64 device, u32 b, vm::ps3::ptr<u32> fd, u32 d);
s32 sys_storage_close(u32 fd);
s32 sys_storage_read();
s32 sys_storage_write();
s32 sys_storage_send_device_command();
s32 sys_storage_async_configure();
s32 sys_storage_async_read();
s32 sys_storage_async_write();
s32 sys_storage_async_cancel();
s32 sys_storage_get_device_info(u64 device, vm::ps3::ptr<u8> buffer);
s32 sys_storage_get_device_config(u64 device, vm::ps3::ptr<u32> count);
s32 sys_storage_report_devices(vm::ps3::ptr<void> a, u32 b, u32 count, vm::ps3::ptr<u64> device_ids);
s32 sys_storage_configure_medium_event(u32 fd, u32 equeue_id, u32 c);
s32 sys_storage_set_medium_polling_interval();
s32 sys_storage_create_region();
s32 sys_storage_delete_region();
s32 sys_storage_execute_device_command(u32 fd, u32 b, vm::ps3::ptr<char> c, u32 d, vm::ps3::ptr<char> e, u32 f, vm::ps3::ptr<u32> driver_status);
s32 sys_storage_check_region_acl();
s32 sys_storage_set_region_acl();
s32 sys_storage_async_send_device_command();
s32 sys_storage_get_region_offset();
s32 sys_storage_set_emulated_speed();