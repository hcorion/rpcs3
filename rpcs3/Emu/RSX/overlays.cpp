#include "stdafx.h"
#include "overlays.h"
#include "GSRender.h"

namespace rsx
{
	namespace overlays
	{
		void user_interface::close()
		{
			//Force unload
			exit = true;
			if (auto rsxthr = fxm::get<GSRender>())
				rsxthr->shell_close_dialog();

			if (on_close)
				on_close(return_code);
		}

		void user_interface::refresh()
		{
			if (auto rsxthr = fxm::get<GSRender>())
			{
				rsxthr->native_ui_flip_request.store(true);
				std::this_thread::sleep_for(16ms);
			}
		}
	}
}
