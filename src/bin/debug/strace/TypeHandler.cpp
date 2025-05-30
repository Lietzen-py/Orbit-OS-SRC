/*
 * Copyright 2005-2007, Haiku Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 * 		Ingo Weinhold <bonefish@cs.tu-berlin.de>
 * 		Hugo Santos <hugosantos@gmail.com>
 */

#include "TypeHandler.h"

#include <string.h>
#include <String.h>
#include <sys/uio.h>

#include "Context.h"
#include "MemoryReader.h"
#include "Syscall.h"


template<typename value_t>
static inline value_t
get_value(const void *address)
{
	if (sizeof(align_t) > sizeof(value_t))
		return value_t(*(align_t*)address);
	else
		return *(value_t*)address;
}


// #pragma mark -


// const void *
TypeHandler *
create_pointer_type_handler()
{
	return new TypeHandlerImpl<const void*>();
}


// const char *
TypeHandler *
create_string_type_handler()
{
	return new TypeHandlerImpl<const char*>();
}


// #pragma mark - complete specializations


// void
template<>
string
TypeHandlerImpl<void>::GetParameterValue(Context &, Parameter *, const void *)
{
	return "void";
}


template<>
string
TypeHandlerImpl<void>::GetReturnValue(Context &, uint64 value)
{
	return "";
}


template<>
TypeHandler *
TypeHandlerFactory<void>::Create()
{
	return new TypeHandlerImpl<void>();
}


// bool
template<>
string
TypeHandlerImpl<bool>::GetParameterValue(Context &, Parameter *,
					 const void *address)
{
	return (*(const align_t*)address ? "true" : "false");
}

template<>
string
TypeHandlerImpl<bool>::GetReturnValue(Context &, uint64 value)
{
	return (value ? "true" : "false");
}

template<>
TypeHandler *
TypeHandlerFactory<bool>::Create()
{
	return new TypeHandlerImpl<bool>();
}


// status_t
template<typename T>
class StatusTypeHandler : public TypeHandler {
public:
	StatusTypeHandler() {}

	string GetParameterValue(Context &context, Parameter *, const void *address)
	{
		return RenderValue(context, get_value<T>(address));
	}

	string GetReturnValue(Context &context, uint64 value)
	{
		return RenderValue(context, value);
	}

private:
	string RenderValue(Context &context, uint64 value) const
	{
		string rendered = context.FormatUnsigned(value);
		if ((T)value <= 0) {
			rendered += " ";
			rendered += strerror(value);
		}
		return rendered;
	}
};

TypeHandler *
create_status_t_type_handler()
{
	return new StatusTypeHandler<status_t>;
}

TypeHandler *
create_ssize_t_type_handler()
{
	return new StatusTypeHandler<ssize_t>;
}


// read_string
static
string
read_string(Context &context, void *data)
{
	if (data == NULL || !context.GetContents(Context::STRINGS))
		return context.FormatPointer(data);

	char buffer[256];
	int32 bytesRead;
	status_t error = context.Reader().Read(data, buffer, sizeof(buffer), bytesRead);
	if (error == B_OK) {
		string result("\"");
		result += string(buffer, strnlen(buffer, sizeof(buffer)));
		result += "\"";
		return result;
	}

	return context.FormatPointer(data) + " (" + strerror(error) + ")";
}

// const void*
template<>
string
TypeHandlerImpl<const void*>::GetParameterValue(Context &context, Parameter *,
						const void *address)
{
	return context.FormatPointer(*(void **)address);
}

template<>
string
TypeHandlerImpl<const void*>::GetReturnValue(Context &context, uint64 value)
{
	return context.FormatPointer((void *)value);
}

// const char*
template<>
string
TypeHandlerImpl<const char*>::GetParameterValue(Context &context, Parameter *,
						const void *address)
{
	return read_string(context, *(void **)address);
}

template<>
string
TypeHandlerImpl<const char*>::GetReturnValue(Context &context, uint64 value)
{
	return read_string(context, (void *)value);
}


// #pragma mark - enums, flags, enum_flags


EnumTypeHandler::EnumTypeHandler(const EnumMap &m) : fMap(m) {}

string
EnumTypeHandler::GetParameterValue(Context &context, Parameter *,
				   const void *address)
{
	return RenderValue(context, get_value<unsigned int>(address));
}

string
EnumTypeHandler::GetReturnValue(Context &context, uint64 value)
{
	return RenderValue(context, value);
}

string
EnumTypeHandler::RenderValue(Context &context, unsigned int value) const
{
	if (context.GetContents(Context::ENUMERATIONS)) {
		EnumMap::const_iterator i = fMap.find(value);
		if (i != fMap.end() && i->second != NULL)
			return i->second;
	}

	return context.FormatUnsigned(value);
}

FlagsTypeHandler::FlagsTypeHandler(const FlagsList &m) : fList(m) {}

string
FlagsTypeHandler::GetParameterValue(Context &context, Parameter *,
	const void *address)
{
	return RenderValue(context, get_value<unsigned int>(address));
}

string
FlagsTypeHandler::GetReturnValue(Context &context, uint64 value)
{
	return RenderValue(context, value);
}

string
FlagsTypeHandler::RenderValue(Context &context, unsigned int value) const
{
	if (context.GetContents(Context::ENUMERATIONS)) {
		// Enumerate the list in reverse. That way, any later values which use
		// the same bits as earlier values will be processed correctly.
		string rendered;
		FlagsList::const_reverse_iterator i = fList.rbegin();
		for (; i != fList.rend(); i++) {
			if (value == 0)
				break;
			if ((value & i->value) != i->value)
				continue;

			if (!rendered.empty())
				rendered.insert(0, "|");
			rendered.insert(0, i->name);
			value &= ~(i->value);
		}
		if (value != 0) {
			if (!rendered.empty())
				rendered += "|";

			char hex[20];
			snprintf(hex, sizeof(hex), "0x%x", value);
			rendered += hex;
		}
		if (rendered.empty())
			rendered = "0";
		return rendered;
	}

	return context.FormatUnsigned(value);
}


EnumFlagsTypeHandler::EnumFlagsTypeHandler(const EnumMap &m, const FlagsTypeHandler::FlagsList &l)
	:
	EnumTypeHandler(m), fList(l) {}


string
EnumFlagsTypeHandler::RenderValue(Context &context, unsigned int value) const
{
	if (context.GetContents(Context::ENUMERATIONS)) {
		string rendered;
		FlagsTypeHandler::FlagsList::const_reverse_iterator i = fList.rbegin();
		for (; i != fList.rend(); i++) {
			if (value == 0)
				break;
			if ((value & i->value) != i->value)
				continue;

			if (!rendered.empty())
				rendered.insert(0, "|");
			rendered.insert(0, i->name);
			value &= ~(i->value);
		}

		EnumMap::const_iterator j = fMap.find(value);
		if (j != fMap.end() && j->second != NULL) {
			if (!rendered.empty())
				rendered.insert(0, "|");
			rendered.insert(0, j->second);
		}

		return rendered;
	}

	return context.FormatUnsigned(value);
}

TypeHandlerSelector::TypeHandlerSelector(const SelectMap &m, int sibling,
					 TypeHandler *def)
	: fMap(m), fSibling(sibling), fDefault(def) {}

string
TypeHandlerSelector::GetParameterValue(Context &context, Parameter *param,
				       const void *address)
{
	TypeHandler *target = fDefault;

	int index = get_value<int>(context.GetValue(context.GetSibling(fSibling)));

	SelectMap::const_iterator i = fMap.find(index);
	if (i != fMap.end())
		target = i->second;

	return target->GetParameterValue(context, param, address);
}

string
TypeHandlerSelector::GetReturnValue(Context &context, uint64 value)
{
	return fDefault->GetReturnValue(context, value);
}

template<typename Type>
static bool
obtain_pointer_data(Context &context, Type *data, void *address, uint32 what)
{
	if (address == NULL || !context.GetContents(what))
		return false;

	int32 bytesRead;

	status_t err = context.Reader().Read(address, data, sizeof(Type), bytesRead);
	if (err != B_OK || bytesRead < (int32)sizeof(Type))
		return false;

	return true;
}

template<typename Type>
static string
format_signed_integer_pointer(Context &context, void *address, uint32 count)
{
	Type data;

	if (obtain_pointer_data(context, &data, address, Context::POINTER_VALUES)) {
		string formatted = "[" + context.FormatSigned(data);
		for (uint32 i = 1; i < count; i++) {
			address = (void*)((Type*)address + 1);
			obtain_pointer_data(context, &data, address, Context::POINTER_VALUES);
			formatted += ", " + context.FormatSigned(data);
		}
		formatted += "]";
		return formatted;
	}
	return context.FormatPointer(address);
}

template<typename Type>
static string
format_unsigned_integer_pointer(Context &context, void *address, uint32 count)
{
	Type data;

	if (obtain_pointer_data(context, &data, address, Context::POINTER_VALUES)) {
		string formatted = "[" + context.FormatUnsigned(data);
		for (uint32 i = 1; i < count; i++) {
			address = (void*)((Type*)address + 1);
			obtain_pointer_data(context, &data, address, Context::POINTER_VALUES);
			formatted += ", " + context.FormatUnsigned(data);
		}
		formatted += "]";
		return formatted;
	}

	return context.FormatPointer(address);
}

template<typename Type>
class SignedIntegerTypeHandler : public TypeHandler {
public:
	string GetParameterValue(Context &context, Parameter *,
				 const void *address)
	{
		return context.FormatSigned(get_value<Type>(address), sizeof(Type));
	}

	string GetReturnValue(Context &context, uint64 value)
	{
		return context.FormatSigned(value, sizeof(Type));
	}
};

template<typename Type>
class UnsignedIntegerTypeHandler : public TypeHandler {
public:
	string GetParameterValue(Context &context, Parameter *,
				 const void *address)
	{
		return context.FormatUnsigned(get_value<Type>(address));
	}

	string GetReturnValue(Context &context, uint64 value)
	{
		return context.FormatUnsigned(value);
	}
};

template<typename Type>
class SignedIntegerPointerTypeHandler : public TypeHandler {
	string GetParameterValue(Context &context, Parameter *parameter,
				 const void *address)
	{
		return format_signed_integer_pointer<Type>(context, *(void **)address,
			parameter->Count());
	}

	string GetReturnValue(Context &context, uint64 value)
	{
		return format_signed_integer_pointer<Type>(context, (void *)value, 1);
	}
};

template<typename Type>
class UnsignedIntegerPointerTypeHandler : public TypeHandler {
	string GetParameterValue(Context &context, Parameter *parameter,
				 const void *address)
	{
		return format_unsigned_integer_pointer<Type>(context, *(void **)address,
			parameter->Count());
	}

	string GetReturnValue(Context &context, uint64 value)
	{
		return format_unsigned_integer_pointer<Type>(context, (void *)value, 1);
	}
};

#define SIGNED_INTEGER_TYPE(type) \
	template<> \
	TypeHandler * \
	TypeHandlerFactory<type>::Create() \
	{ \
		return new SignedIntegerTypeHandler<type>(); \
	}

#define UNSIGNED_INTEGER_TYPE(type) \
	template<> \
	TypeHandler * \
	TypeHandlerFactory<type>::Create() \
	{ \
		return new UnsignedIntegerTypeHandler<type>(); \
	}

#define SIGNED_INTEGER_POINTER_TYPE(name, type) \
	TypeHandler *create_##name##_type_handler() \
	{ \
		return new SignedIntegerPointerTypeHandler<type>(); \
	}

#define UNSIGNED_INTEGER_POINTER_TYPE(name, type) \
	TypeHandler *create_##name##_type_handler() \
	{ \
		return new UnsignedIntegerPointerTypeHandler<type>(); \
	}


SIGNED_INTEGER_TYPE(char);
SIGNED_INTEGER_TYPE(short);
SIGNED_INTEGER_TYPE(int);
SIGNED_INTEGER_TYPE(long);
SIGNED_INTEGER_TYPE(long long);

UNSIGNED_INTEGER_TYPE(unsigned char);
UNSIGNED_INTEGER_TYPE(unsigned short);
UNSIGNED_INTEGER_TYPE(unsigned int);
UNSIGNED_INTEGER_TYPE(unsigned long);
UNSIGNED_INTEGER_TYPE(unsigned long long);

SIGNED_INTEGER_POINTER_TYPE(int_ptr, int);
SIGNED_INTEGER_POINTER_TYPE(long_ptr, long);
SIGNED_INTEGER_POINTER_TYPE(longlong_ptr, long long);

UNSIGNED_INTEGER_POINTER_TYPE(uint_ptr, unsigned int);
UNSIGNED_INTEGER_POINTER_TYPE(ulong_ptr, unsigned long);
UNSIGNED_INTEGER_POINTER_TYPE(ulonglong_ptr, unsigned long long);

