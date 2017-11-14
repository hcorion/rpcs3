#include "stdafx.h"
/*
#include "Emu/System.h"
#include "Emu/IdManager.h"
#include "Emu/Cell/PPUModule.h"
#include "Emu/Cell/lv2/sys_mutex.h"
#include "Emu/Cell/lv2/sys_cond.h"
#include "Emu/Cell/PPUOpcodes.h"

#include "Emu/Cell/Modules/cxml.h"

#include "Emu/Cell/Modules/cellSysutil.h"

#include "Utilities/StrUtil.h"

#include <mutex>
#include <queue>

struct subroutine
{
	const char * name;
	s32 attr_num;
	void * func;
};
subroutine subroutines[] = { {"7", 1, nullptr}/*
, "2", "3", "q", "4", "5", "b", "g", "k", "m", "j", "n", "a", "l", "8", "9", "h", "i", "f", "1", "o", "p", "init", "term" /};

s32 processIncomingPacket(cpu_thread& cpu, vm::cptr<char> buffer, u32 buffer_size, cxml_document & output_doc)
{
	cxml_document input_doc;
	input_doc.CreateFromBuffer(buffer, buffer_size, AccessMode_ReadWrite);
	if (input_doc.GetSize() == buffer_size)
	{
		return 0x8002b003;
	}

	cxml_element document_elem = input_doc.GetDocumentElement();
	/*if (*6f87a1 != 0)
	{
		return 0x80022925;
	}/

	for (auto subroutine : subroutines)
	{
		if (cxml_util::CheckElementName(document_elem, subroutine.name))
		{
			cxml_element result_element;
			//output_doc.CreateElement(cpu, vm::make_str("result"), subroutine.attr_num, &result_element);
			// RUN CODE HERE
			return 0x80022d20;
		}
	}

	return 0;
}

s32 handleNPTRPacket(cpu_thread& cpu, vm::cptr<char> buffer, u32 buffer_size)
{
	cxml_document output_doc;
	output_doc.SetHeaderMagic("NPTR");
	s32 result = processIncomingPacket(cpu, buffer, buffer_size);
	return result;
}*/