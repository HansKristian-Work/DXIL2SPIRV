/*
 * Copyright 2019-2020 Hans-Kristian Arntzen for Valve Corporation
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "module.hpp"
#include "logging.hpp"
#include "llvm_decoder.h"
#include <stdlib.h>

namespace llvm
{
enum class KnownBlocks : uint32_t
{
	BLOCKINFO = 0,

	// 1-7 reserved,

	MODULE_BLOCK = 8,
	PARAMATTR_BLOCK = 9,
	PARAMATTR_GROUP_BLOCK = 10,
	CONSTANTS_BLOCK = 11,
	FUNCTION_BLOCK = 12,
	TYPE_SYMTAB_BLOCK = 13,
	VALUE_SYMTAB_BLOCK = 14,
	METADATA_BLOCK = 15,
	METADATA_ATTACHMENT = 16,
	TYPE_BLOCK = 17,
};

enum class ModuleRecord : uint32_t
{
	VERSION = 1,
	TRIPLE = 2,
	DATALAYOUT = 3,
	FUNCTION = 8,
};

enum class ConstantsRecord : uint32_t
{
	SETTYPE = 1,
	CONST_NULL = 2,
	UNDEF = 3,
	INTEGER = 4,
	WIDE_INTEGER = 5,
	FLOAT = 6,
	AGGREGATE = 7,
	STRING = 8,
	DATA = 22,
};

enum class FunctionRecord : uint32_t
{
	DECLAREBLOCKS = 1,
	INST_BINOP = 2,
	INST_CAST = 3,
	INST_GEP_OLD = 4,
	INST_SELECT = 5,
	INST_EXTRACTELT = 6,
	INST_INSERTELT = 7,
	INST_SHUFFLEVEC = 8,
	INST_CMP = 9,
	INST_RET = 10,
	INST_BR = 11,
	INST_SWITCH = 12,
	INST_INVOKE = 13,
	INST_UNREACHABLE = 15,
	INST_PHI = 16,
	INST_ALLOCA = 19,
	INST_LOAD = 20,
	INST_VAARG = 23,
	INST_STORE_OLD = 24,
	INST_EXTRACTVAL = 26,
	INST_INSERTVAL = 27,
	INST_CMP2 = 28,
	INST_VSELECT = 29,
	INST_INBOUNDS_GEP_OLD = 30,
	INST_INDIRECTBR = 31,
	DEBUG_LOC_AGAIN = 33,
	INST_CALL = 34,
	DEBUG_LOC = 35,
	INST_FENCE = 36,
	INST_CMPXCHG_OLD = 37,
	INST_ATOMICRMW = 38,
	INST_RESUME = 39,
	INST_LANDINGPAD_OLD = 40,
	INST_LOADATOMIC = 41,
	INST_STOREATOMIC_OLD = 42,
	INST_GEP = 43,
	INST_STORE = 44,
	INST_STOREATOMIC = 45,
	INST_CMPXCHG = 46,
	INST_LANDINGPAD = 47,
	INST_CLEANUPRET = 48,
	INST_CATCHRET = 49,
	INST_CATCHPAD = 50,
	INST_CLEANUPPAD = 51,
	INST_CATCHSWITCH = 52,
	OPERAND_BUNDLE = 55,
	INST_UNOP = 56,
	INST_CALLBR = 57,
};

enum class ValueSymtabRecord : uint32_t
{
	ENTRY = 1,
	BBENTRY = 2,
	FNENTRY = 3,
	COMBINED_ENTRY = 5,
};

enum class MetaDataRecord : uint32_t
{
	STRING_OLD = 1,
	VALUE = 2,
	NODE = 3,
	NAME = 4,
	DISTINCT_NODE = 5,
	KIND = 6,
	LOCATION = 7,
	OLD_NODE = 8,
	OLD_FN_NODE = 9,
	NAMED_NODE = 10,
	ATTACHMENT = 11,
	GENERIC_DEBUG = 12,
	SUBRANGE = 13,
	ENUMERATOR = 14,
	BASIC_TYPE = 15,
	FILE = 16,
	DERIVED_TYPE = 17,
	COMPOSITE_TYPE = 18,
	SUBROUTINE_TYPE = 19,
	COMPILE_UNIT = 20,
	SUBPROGRAM = 21,
	LEXICAL_BLOCK = 22,
	LEXICAL_BLOCK_FILE = 23,
	NAMESPACE = 24,
	TEMPLATE_TYPE = 25,
	TEMPLATE_VALUE = 26,
	GLOBAL_VAR = 27,
	LOCAL_VAR = 28,
	EXPRESSION = 29,
	OBJC_PROPERTY = 30,
	IMPORTED_ENTITY = 31,
	MODULE = 32,
	MACRO = 33,
	MACRO_FILE = 34,
	STRINGS = 35,
	GLOBAL_DECL_ATTACHMENT = 36,
	GLOBAL_VAR_EXPR = 37,
	INDEX_OFFSET = 38,
	INDEX = 39,
	LABEL = 40,
	COMMON_BLOCK = 44,
};

enum class TypeRecord : uint32_t
{
	NUMENTRY = 1,
	VOID = 2,
	FLOAT = 3,
	DOUBLE = 4,
	LABEL = 5,
	OPAQUE = 6,
	INTEGER = 7,
	POINTER = 8,
	FUNCTION_OLD = 9,
	HALF = 10,
	ARRAY = 11,
	VECTOR = 12,
	METADATA = 16,
	STRUCT_ANON = 18,
	STRUCT_NAME = 19,
	STRUCT_NAMED = 20,
	FUNCTION = 21,
	TOKEN = 22,
};

LLVMContext::LLVMContext()
{
}

LLVMContext::~LLVMContext()
{
	for (size_t i = typed_allocations.size(); i; i--)
		typed_allocations[i - 1]->run();
	for (size_t i = raw_allocations.size(); i; i--)
		free(raw_allocations[i - 1]);
}

void *LLVMContext::allocate(size_t size, size_t /*align*/)
{
	// TODO: Chain allocation.
	void *ptr = malloc(size);
	raw_allocations.push_back(ptr);
	return ptr;
}

static bool is_known(uint32_t id, KnownBlocks block)
{
	return id == uint32_t(block);
}

static void parse_value_symtab(Module *module, const BlockOrRecord &entry)
{
	for (auto &symtab : entry.children)
	{
		auto name = symtab.getString(1);
		module->add_function_name(symtab.ops[0], name);
		LOGI("Function %u is \"%s\"\n", unsigned(symtab.ops[0]), name.c_str());
	}
}

static void parse_function(Module *module, const BlockOrRecord &entry)
{

}

static void parse_type(Module *module, const BlockOrRecord &child)
{
	auto &context = module->getContext();
	TypeID id = TypeID::Unknown;

	switch (TypeRecord(child.id))
	{
	case TypeRecord::VOID:
		id = TypeID::Void;
		break;

	case TypeRecord::HALF:
		id = TypeID::Half;
		break;

	case TypeRecord::FLOAT:
		id = TypeID::Float;
		break;

	case TypeRecord::DOUBLE:
		id = TypeID::Double;
		break;

	case TypeRecord::POINTER:
	{
		auto *type = context.construct<PointerType>(*module, module->get_type(child.ops[0]), child.ops[1]);
		module->add_type(type);
		return;
	}

	case TypeRecord::ARRAY:
	{
		auto *type = context.construct<ArrayType>(*module, module->get_type(child.ops[1]), child.ops[0]);
		module->add_type(type);
		return;
	}

	case TypeRecord::INTEGER:
	{
		auto *type = context.construct<IntegerType>(*module, child.ops[0]);
		module->add_type(type);
		return;
	}

	case TypeRecord::STRUCT_NAMED:
	case TypeRecord::STRUCT_ANON:
	{
		auto *type = context.construct<StructType>(*module);
		for (size_t i = 1; i < child.ops.size(); i++)
			type->add_member_type(module->get_type(child.ops[i]));
		module->add_type(type);
		return;
	}

	case TypeRecord::FUNCTION:
	{
		auto *type = context.construct<FunctionType>(*module);
		type->set_return_type(module->get_type(child.ops[1]));
		for (size_t i = 2; i < child.ops.size(); i++)
			type->add_argument_type(module->get_type(child.ops[i]));
		module->add_type(type);
		return;
	}

	default:
		break;
	}

	auto *type = context.construct<Type>(*module, id);
	module->add_type(type);
}

static void parse_types(Module *module, const BlockOrRecord &entry)
{
	for (auto &child : entry.children)
		parse_type(module, child);
}

Module *parseIR(LLVMContext &context, const void *data, size_t size)
{
	LLVMBC::BitcodeReader reader(static_cast<const uint8_t *>(data), size);
	auto toplevel = reader.ReadToplevelBlock();

	// The top-level block must be MODULE_BLOCK.
	if (KnownBlocks(toplevel.id) != KnownBlocks::MODULE_BLOCK)
		return nullptr;

	// We should have consumed all bits, only one top-level block.
	if (!reader.AtEndOfStream())
		return nullptr;

	auto *module = context.construct<Module>(context);

	for (auto &child : toplevel.children)
	{
		if (child.IsBlock() && is_known(child.id, KnownBlocks::VALUE_SYMTAB_BLOCK))
			parse_value_symtab(module, child);
		if (child.IsBlock() && is_known(child.id, KnownBlocks::FUNCTION_BLOCK))
			parse_function(module, child);
		if (child.IsBlock() && is_known(child.id, KnownBlocks::TYPE_BLOCK))
			parse_types(module, child);
	}

	return module;
}

Type *Module::get_type(uint32_t index)
{
	assert(index < types.size());
	return types[index];
}

void Module::add_type(Type *type)
{
	types.push_back(type);
}

void Module::add_function_name(uint64_t id, const std::string &name)
{
	value_symtab[id] = name;
}

LLVMContext &Module::getContext()
{
	return context;
}

Module::Module(LLVMContext &context_)
	: context(context_)
{
}

void StructType::add_member_type(Type *type)
{
	member_types.push_back(type);
}

void FunctionType::add_argument_type(Type *argument_type)
{
	argument_types.push_back(argument_type);
}

void FunctionType::set_return_type(Type *return_type_)
{
	return_type = return_type_;
}

PointerType::PointerType(Module &module, Type *type, uint32_t addr_space)
	: Type(module, TypeID::Pointer), contained_type(type), address_space(addr_space)
{
}

ArrayType::ArrayType(Module &module, Type *type, uint32_t elements_)
	: Type(module, TypeID::Array), contained_type(type), elements(elements_)
{
}

StructType::StructType(Module &module)
	: Type(module, TypeID::Struct)
{
}

FunctionType::FunctionType(LLVMBC::Module &module)
	: Type(module, TypeID::Function)
{
}

IntegerType::IntegerType(Module &module, uint32_t width_)
	: Type(module, TypeID::Int), width(width_)
{
}

Type::Type(Module &module_, TypeID type_id_)
	: module(module_), type_id(type_id_)
{
}

}
