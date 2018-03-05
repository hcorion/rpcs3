#pragma once

#include "Emu/Memory/Memory.h"

namespace vm { using namespace ps3; }

using CXmlAllocator = void(u32 allocType, vm::ptr<void> userData, vm::ptr<void> oldAddr, u32 requiredSize, vm::ptr<void> addr, vm::ptr<u32> size);

#define CXML_TO_CELLSYSUTIL_ERROR(error) ((error) ^ 0x80550000 ^ 0x8002E000)

// TODO switch all of these to values cellsysutil would later switch
enum
{
	// guessing with these
	CELL_CXML_INVALID_BUFFER_SIZE = 0x80550002,
	CELL_CXML_INVALID_OFFSET = 0x80550004,
	CELL_CXML_ALLOCATION_ERROR = 0x80550005,
	CELL_CXML_INVALID_TABLE = 0x80550006,
	CELL_CXML_INVALID_VERSION = 0x80550007,
	CELL_CXML_INVALID_DOC = 0x80550008,
	CELL_CXML_INVALID_ACCESS_MODE = 0x80550010,
	CELL_CXML_ELEMENT_CANT_FIND_ATTRIBUTE = 0x80550011,
	CELL_CXML_ELEMENT_INVALID_ATTRIBUTE_NUM = 0x80550012,
	CELL_CXML_ELEMENT_INVALID_OFFSET = 0x80550013,
	CELL_CXML_NO_ALLOCATOR = 0x80550014,
};

enum Cxml_AllocationType
{
	AllocationType_Alloc_Tree,
	AllocationType_Alloc_IDTable,
	AllocationType_Alloc_StringTable,
	AllocationType_Alloc_IntArrayTable,
	AllocationType_Alloc_FloatArrayTable,
	AllocationType_Alloc_FileTable,

	AllocationType_Free_Tree,
	AllocationType_Free_IDTable,
	AllocationType_Free_StringTable,
	AllocationType_Free_IntArrayTable,
	AllocationType_Free_FloatArrayTable,
	AllocationType_Free_FileTable,
};

enum AccessMode
{
	AccessMode_ReadWrite,
	AccessMode_ReadOnly_File,
	AccessMode_ReadOnly_Memory,
};

struct cxml_childelement_bin;
struct cxml_element;

struct cxml_document
{
	be_t<u32> accessMode;                 // 0x00
	struct Header {
		Header();

		char magic[4];                    // 0x04
		be_t<u32> version;                // 0x08
		be_t<u32> tree_offset;            // 0x0C
		be_t<u32> tree_size;              // 0x10
		be_t<u32> idtable_offset;         // 0x14
		be_t<u32> idtable_size;           // 0x18
		be_t<u32> stringtable_offset;     // 0x1C
		be_t<u32> stringtable_size;       // 0x20
		be_t<u32> intarraytable_offset;   // 0x24
		be_t<u32> intarraytable_size;     // 0x28
		be_t<u32> floatarraytable_offset; // 0x2C
		be_t<u32> floatarraytable_size;   // 0x30
		be_t<u32> filetable_offset;       // 0x34
		be_t<u32> filetable_size;         // 0x38
	} header;

	vm::bptr<u8> tree;               // 0x3C
	be_t<u32> tree_capacity;           // 0x40
	vm::bptr<u8> idtable;            // 0x44
	be_t<u32> idtable_capacity;        // 0x48
	vm::bptr<char> stringtable;        // 0x4C
	be_t<u32> stringtable_capacity;    // 0x50
	vm::bptr<u32> intarraytable;       // 0x54
	be_t<u32> intarraytable_capacity;  // 0x58
	vm::bptr<f32> floatarraytable;   // 0x5C
	be_t<u32> floatarraytable_capacity;// 0x60
	vm::bptr<u8> filetable;          // 0x64
	be_t<u32> filetable_capacity;      // 0x68
	vm::bptr<CXmlAllocator> allocator; // 0x6C
	vm::bptr<void> allocator_userdata; // 0x70

	cxml_document();
	~cxml_document();
	char * GetString(s32 offset);
	s32 CreateFromBuffer(vm::cptr<char> buf, u32 buf_size, AccessMode access_mode);
	s32 GetSize() const;
	s32 SetHeaderMagic(const char buf[4]);
	s32 Clear();
	s32 RegisterString(vm::cptr<char> name, u32 nameLen);
	s32 CreateElement(vm::cptr<char> name, s32 attr_num, cxml_element * element);
	bool IsValidElement(s32 offset);

	cxml_element GetDocumentElement();

	s32 AddOrGetStringOffset(/*ppu_thread& ppu, */vm::cptr<char> string, u32 length);
};

static_assert(sizeof(cxml_document) == 0x74, "Invalid CXml doc size!");

static_assert(sizeof(cxml_document::Header) == 0x38, "Invalid CXml header size!");

struct cxml_element
{
	cxml_element(vm::bptr<cxml_document> new_doc, u32 new_offset) :
		doc(new_doc),
		offset(new_offset)
	{}

	cxml_element() :
		doc(vm::null),
		offset(0)
	{}

	vm::bptr<cxml_document> doc;
	be_t<u32> offset;


	const char * GetName() const;
};

struct cxml_attribute
{
	vm::bptr<cxml_document> doc;
	be_t<u32> element_offset;
	be_t<u32> offset;
};

struct cxml_childelement_bin
{
	be_t<s32> name;        // 0x00
	be_t<s32> attr_num;    // 0x04
	be_t<s32> parent;      // 0x08
	be_t<s32> prev;        // 0x0C
	be_t<s32> next;        // 0x10
	be_t<s32> first_child; // 0x14
	be_t<s32> last_child;  // 0x18
};

struct cxml_attribute_bin
{
	be_t<s32> name;        // 0x00
	be_t<s32> type;		   // 0x04

	union
	{
		be_t<s32> int32;
		be_t<float> float32;
		struct
		{
			be_t<s32> offset;
			be_t<s32> length;
		} string_data;

		struct
		{
			be_t<s32> offset;
			be_t<s32> number;
		} int_array_data;

		struct
		{
			be_t<s32> offset;
			be_t<s32> number;
		} float_array_data;

		struct
		{
			be_t<s32> offset;
			be_t<s32> size;
		} file_data;

		be_t<s32> id;
		be_t<s32> idref;
	};
};

static_assert(sizeof(cxml_childelement_bin) == 0x1C, "Invalid CXml child element bin size!");
/*
namespace cxml_util
{
	bool CheckElementName(const cxml_element & elm, const char * valid_name)
	{
		const char * name = elm.GetName();

		if (name[0] == valid_name[0] && strcmp(name, valid_name) == 0)
		{
			return true;
		}
		else
		{
			return false;
		}
	}
}*/