#include "stdafx.h"
#include "Emu/Memory/Memory.h"
#include "Emu/System.h"

#include "Emu/Cell/ErrorCodes.h"
#include "sys_config.h"

namespace vm { using namespace ps3; }

logs::channel sys_config("sys_config");

error_code sys_config_open(u32 equeue_id, vm::ptr<u32> config_id)
{
	sys_config.warning("sys_config_open(equeue_id=0x%x, config_id=*0x%x)", equeue_id, config_id);

	const auto queue = idm::get<lv2_obj, lv2_event_queue>(equeue_id);

	if (!queue)
	{
		return CELL_ESRCH;
	}

	auto config = std::make_shared<lv2_config>();
	if (const u32 id = idm::import_existing<lv2_config>(std::move(config)))
	{
		config->queue = std::move(queue);
		*config_id = id;

		return CELL_OK;
	}

	return CELL_EAGAIN;
}

error_code sys_config_close(u32 config_id)
{
	sys_config.warning("sys_config_close(config_id=0x%x)", config_id);

	if (!idm::remove<lv2_config>(config_id))
	{
		return CELL_ESRCH;
	}

	return CELL_OK;
}

error_code sys_config_register_service(u32 config_id, s32 b, u32 c, u32 d, vm::ptr<u32> data, u32 size, vm::ptr<u32> output)
{
	// `size` is the length of `data`
	sys_config.todo("sys_config_register_service(config_id=0x%x, 0x%x, 0x%x, 0x%x, data=0x%x, size=0x%x, output=0x%x)", config_id, b, c, d, data, size, output);

	if (b < 0)
	{
		return CELL_EINVAL;
	}

	*output = 0xcafebabe;

	return CELL_OK;
}

error_code sys_config_add_service_listener(u32 config_id, s32 id, u32 c, u32 d, u32 e, u32 f, u32 g)
{
	sys_config.todo("sys_config_add_service_listener(config_id=0x%x, id=0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x)", config_id, id, c, d, e, f, g);
	return CELL_OK;
}