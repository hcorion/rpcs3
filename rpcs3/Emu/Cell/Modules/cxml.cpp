#include "stdafx.h"
#include "Emu/System.h"

#include "cxml.h"

namespace vm { using namespace ps3; }

cxml_document::cxml_document()
{
	memset(this, 0, sizeof(*this));
	accessMode = AccessMode_ReadWrite;
	allocator.set(0xdeadf00d); // We use vm::alloc, we trace whether the allocator is null with the magic
	header = Header();
}

cxml_document::~cxml_document()
{
	Clear();
}

char * cxml_document::GetString(s32 offset)
{
	if (offset < 0 || offset >= header.stringtable_size)
	{
		return nullptr;
	}

	return &stringtable[offset];
}

s32 cxml_document::CreateFromBuffer(vm::cptr<char> buf, u32 buf_size, AccessMode access_mode)
{
	/*if (access_mode != AccessMode_ReadWrite && accessMode != AccessMode_ReadOnly_Memory)
	{
		return CELL_CXML_INVALID_ACCESS_MODE;
	}

	// clear

	accessMode = access_mode;

	if (buf_size < sizeof(header))
	{
		return CELL_CXML_INVALID_BUFFER_SIZE;
	}

	memcpy(&header, buf.get_ptr(), sizeof(header));

	if (header.version != 0x110 && header.version != 0x100)
	{
		return CELL_CXML_INVALID_VERSION;
	}

	if (header.intarraytable_size % sizeof(intarraytable[0]) != 0)
	{
		return CELL_CXML_INVALID_TABLE;
	}

	if (header.floatarraytable_size % sizeof(floatarraytable[0]) != 0)
	{
		return CELL_CXML_INVALID_TABLE;
	}

	if (access_mode == AccessMode_ReadWrite && !allocator)
	{
		return CELL_CXML_NO_ALLOCATOR;
	}

#define initialize_field(field) if (header.##field##_offset >= buf_size) \
		{ \
			return CELL_CXML_ELEMENT_INVALID_OFFSET; \
		} \
		if (header.##field##_offset + header.##field##_size > buf_size) \
		{ \
			return CELL_CXML_INVALID_BUFFER_SIZE; \
		} \
		if (access_mode == AccessMode_ReadWrite) \
		{ \
			/*allocator should replace vm::alloc here* / \
			u32 old_##field = field##.addr(); \
			u32 new_##field = vm::alloc(header.##field##_size, vm::main); \
			if (new_##field == 0) \
			{ \
				return CELL_CXML_ALLOCATION_ERROR; \
			} \
			##field##_capacity = align(u32(header.##field##_size), 0x1000); \
			##field##.set(new_##field##); \
			memcpy(##field##.get_ptr(), buf.get_ptr() + header.##field##_offset, header.##field##_size); \
			if (old_##field) \
			{ \
				/*vm::dealloc(old_##field);* / \
			} \
		} \
		else \
		{ \
			field##.set(buf.addr() + header.tree_offset); \
		}

	if (header.tree_size > 0)
	{
		initialize_field(tree);
	}

	if (header.idtable_size > 0)
	{
		initialize_field(idtable);
	}

	if (header.stringtable_size > 0)
	{
		initialize_field(stringtable);

		if (stringtable[header.stringtable_size - 1] != 0)
		{
			return CELL_CXML_INVALID_TABLE;
		}
	}

	if (header.intarraytable_size > 0)
	{
		initialize_field(intarraytable);
	}

	if (header.floatarraytable_size > 0)
	{
		initialize_field(floatarraytable);
	}

	if (header.filetable_size > 0)
	{
		initialize_field(filetable);
	}

#undef initialize_field
	*/
	return 0;
}

s32 cxml_document::GetSize() const
{
	return ::align(sizeof(header), 16) +
		::align((u32)header.tree_size, 16) +
		::align((u32)header.idtable_size, 16) +
		::align((u32)header.stringtable_size, 16) +
		::align((u32)header.intarraytable_size, 16) +
		::align((u32)header.floatarraytable_size, 16) +
		::align((u32)header.filetable_size, 16);
}

s32 cxml_document::SetHeaderMagic(const char buf[4])
{
	memcpy(header.magic, buf, 4);
	return 0;
}

s32 cxml_document::Clear()
{
	/*header = Header();

	if (accessMode != AccessMode_ReadOnly_Memory)
	{
		if (!allocator)
		{
			return CELL_CXML_NO_ALLOCATOR;
		}

#define free_field(field) if (this->##field) \
	{ \
		vm::dealloc(this->##field##.addr()); \
	} \
	this->##field = vm::null; \
	this->##field##_capacity = 0;

		free_field(tree);
		free_field(idtable);
		free_field(stringtable);
		free_field(intarraytable);
		free_field(floatarraytable);
		free_field(filetable);
#undef free_field
	}

	accessMode = AccessMode_ReadWrite;*/
	return 0;
}

s32 cxml_document::RegisterString(vm::cptr<char> name, u32 nameLen)
{
	/*int i = 0;
	while (i < header.stringtable_size)
	{
		if (std::strcmp(stringtable.get_ptr() + i, name.get_ptr()) == 0)
			return i;
		i += std::strlen(stringtable.get_ptr() + i) + 1;
	}
	// else add at the end
	if (stringtable_capacity < (header.stringtable_size + nameLen + 1))
	{
		const u32 reqSize = header.stringtable_size + nameLen + 1;
		vm::var<u32> size;
		vm::var<u32> addr;

		//allocator(ppu, AllocationType_Alloc_StringTable, allocator_userdata, stringtable, reqSize, addr, size);
		if (addr == vm::null)
		{
			return CELL_CXML_ALLOCATION_ERROR;
		}

		stringtable.set(*addr);
		stringtable_capacity = *size;
	}

	std::memcpy(stringtable.get_ptr() + header.stringtable_size, name.get_ptr(), nameLen + 1);

	const u32 stringOffset = header.stringtable_size;
	header.stringtable_size += nameLen + 1;
	
	return stringOffset;*/
	return 0;
}

s32 cxml_document::CreateElement(vm::cptr<char> name, s32 attr_num, cxml_element * element)
{
	/*if (accessMode != AccessMode_ReadWrite)
	{
		return CELL_CXML_INVALID_ACCESS_MODE;
	}

	if (!allocator)
	{
		return CELL_CXML_NO_ALLOCATOR;
	}

	const u32 neededSize = (attr_num * sizeof(cxml_attribute_bin)) + sizeof(cxml_childelement_bin);
	const u32 nameLen = std::strlen(name.get_ptr());

	if (header.tree_size + neededSize > tree_capacity)
	{
		vm::var<u32> size = 0;
		vm::var<u32> addr = 0;

		//allocator(ppu, AllocationType_Alloc_Tree, allocator_userdata, tree, header.tree_size + neededSize, addr, size);
		if (addr == vm::null)
		{
			return CELL_CXML_ALLOCATION_ERROR;
		}

		tree.set(*addr);
		tree_capacity = *size;
	}

	u32 treeOffset = header.tree_size;
	header.tree_size += neededSize;

	element->offset = treeOffset;
	element->doc.set(vm::get_addr(this));

	const s32 stringOffset = RegisterString(name, nameLen);
	if (stringOffset < 0)
		return stringOffset;

	auto& lastElement = vm::_ref<cxml_childelement_bin>(tree.addr() + treeOffset);

	lastElement.name = stringOffset;
	lastElement.attr_num = attr_num;
	lastElement.parent = -1;
	lastElement.prev = -1;
	lastElement.next = -1;
	lastElement.first_child = -1;
	lastElement.last_child = -1;

	const u32 counter = attr_num < 0 ? 1 : attr_num + 1;

	for (int i = counter; i != 0; --i) {
		auto& newElement = vm::_ref<cxml_childelement_bin>(tree.addr() + treeOffset + sizeof(cxml_childelement_bin));
		newElement.name = -1;
		newElement.attr_num = 0;
		newElement.parent = 0;
		newElement.prev = 0;

		treeOffset += 0x10;
	}
	*/
	return 0;
}

bool cxml_document::IsValidElement(s32 offset) {
	/*if (offset < 0 || offset + sizeof(cxml_childelement_bin) > header.tree_size)
	{
		return false;
	}

	cxml_childelement_bin* bin = reinterpret_cast<cxml_childelement_bin *>(&(tree[offset]));
	if (bin->attr_num < 0 || offset + sizeof(cxml_childelement_bin) + (sizeof(cxml_attribute_bin) * bin->attr_num) > header.tree_size)
	{
		return false;
	}
	*/
	return true;
}

cxml_element cxml_document::GetDocumentElement()
{
	/*if (IsValidElement(0))
	{
		return cxml_element(vm::get_addr(this), 0);
	}*/

	return cxml_element(vm::null, -1);
}

s32 cxml_document::AddOrGetStringOffset(vm::cptr<char> string, u32 length)
{
	/*if (!allocator)
	{
		return CELL_CXML_NO_ALLOCATOR;
	}

	for (u32 i = 0; i < header.stringtable_size; i++)
	{
		u32 it_length = stringtable[i];
		if (memcmp(&stringtable[i], string.get_ptr(), it_length) == 0)
		{
			return i;
		}

		i += it_length;
	}

	if (header.stringtable_size + length + 1 > stringtable_capacity)
	{
		u32 required_capacity = header.stringtable_size + length + 1;

		//allocator(ppu, AllocationType_Alloc_StringTable, allocator_userdata, stringtable, required_capacity, new_stringtable, new_capacity);

		u32 new_stringtable = vm::alloc(required_capacity, vm::main);
		if (new_stringtable == 0)
		{
			return CELL_CXML_ALLOCATION_ERROR;
		}

		memcpy(vm::base(new_stringtable), stringtable.get_ptr(), header.stringtable_size);
		u32 old_address = stringtable.addr();

		this->stringtable.set(new_stringtable);
		this->stringtable_capacity = align(required_capacity, 0x1000)/**new_capacity*/;

		//vm::dealloc(old_address);
	//}
return 0;/*
	s32 offset = header.stringtable_size;
	header.stringtable_size += length + 1;
	memcpy(&stringtable[offset], string.get_ptr(), length + 1);
	
	return length;*/
}

cxml_document::Header::Header()
{
	/*memset(this, 0, sizeof(*this));

	version = 0x110;
	memcpy(magic, "CXML", 4);*/
}

const char * cxml_element::GetName() const
{
	/*if (doc)
	{
		cxml_childelement_bin * bin = (cxml_childelement_bin*)&(doc->tree[offset]);

		return doc->GetString(bin->name);
	}*/

	return nullptr;
}
