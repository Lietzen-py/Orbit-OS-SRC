/*
 * Copyright 2005-2007, Haiku Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 * 		Ingo Weinhold <bonefish@cs.tu-berlin.de>
 * 		Hugo Santos <hugosantos@gmail.com>
 */
#ifndef STRACE_TYPE_HANDLER_H
#define STRACE_TYPE_HANDLER_H

#include <list>
#include <map>
#include <string>

#include <arch_config.h>
#include <SupportDefs.h>

using std::string;

class Context;
class Parameter;
class MemoryReader;

typedef FUNCTION_CALL_PARAMETER_ALIGNMENT_TYPE align_t;

// TypeHandler
class TypeHandler {
public:
	TypeHandler() {}
	virtual ~TypeHandler() {}

	virtual string GetParameterValue(Context &, Parameter *,
					 const void *value) = 0;
	virtual string GetReturnValue(Context &, uint64 value) = 0;
};

class EnumTypeHandler : virtual public TypeHandler {
public:
	typedef std::map<int, const char *> EnumMap;

	EnumTypeHandler(const EnumMap &);

	string GetParameterValue(Context &c, Parameter *, const void *);
	string GetReturnValue(Context &, uint64 value);

	virtual string RenderValue(Context &, unsigned int value) const;

protected:
	const EnumMap &fMap;
};

class FlagsTypeHandler : virtual public TypeHandler {
public:
	struct FlagInfo {
		unsigned int value;
		const char* name;
	};
	typedef std::list<FlagInfo> FlagsList;

	FlagsTypeHandler(const FlagsList &);

	string GetParameterValue(Context &c, Parameter *, const void *);
	string GetReturnValue(Context &, uint64 value);

	virtual string RenderValue(Context &, unsigned int value) const;

private:
	const FlagsList &fList;
};

class EnumFlagsTypeHandler : public EnumTypeHandler {
public:
	EnumFlagsTypeHandler(const EnumMap &, const FlagsTypeHandler::FlagsList &);

	string RenderValue(Context &, unsigned int value) const;

private:
	const FlagsTypeHandler::FlagsList &fList;
};

// currently limited to select ints
class TypeHandlerSelector : public TypeHandler {
public:
	typedef std::map<int, TypeHandler *> SelectMap;

	TypeHandlerSelector(const SelectMap &, int sibling,
			    TypeHandler *def);

	string GetParameterValue(Context &, Parameter *, const void *);
	string GetReturnValue(Context &, uint64 value);

private:
	const SelectMap &fMap;
	int fSibling;
	TypeHandler *fDefault;
};

// templatized TypeHandler factory class
// (I tried a simple function first, but then the compiler complains for
// the partial instantiation. Not sure, if I'm missing something or this is
// a compiler bug).
template<typename Type>
struct TypeHandlerFactory {
	static TypeHandler *Create();
};

extern TypeHandler *create_pointer_type_handler();
extern TypeHandler *create_string_type_handler();
extern TypeHandler *create_status_t_type_handler();
extern TypeHandler *create_ssize_t_type_handler();

// specialization for "const char*"
template<>
struct TypeHandlerFactory<const char*> {
	static inline TypeHandler *Create()
	{
		return create_string_type_handler();
	}
};

#define DEFINE_FACTORY(name, type) \
	template<> \
	struct TypeHandlerFactory<type> { \
		static inline TypeHandler *Create() \
		{ \
			extern TypeHandler *create_##name##_type_handler(); \
			return create_##name##_type_handler(); \
		} \
	} \

#define DEFINE_TYPE(name, type) \
	TypeHandler *create_##name##_type_handler() \
	{ \
		return new TypeHandlerImpl<type>(); \
	}

struct flock;
struct ifconf;
struct ifreq;
struct iovec;
struct msghdr;
struct message_args;
struct sockaddr;
struct sockaddr_args;
struct socket_args;
struct sockopt_args;

struct fd_set;
struct pollfd;
struct object_wait_info;
struct event_wait_info;

struct rlimit;

DEFINE_FACTORY(flock_ptr, flock *);
DEFINE_FACTORY(ifconf_ptr, ifconf *);
DEFINE_FACTORY(ifreq_ptr, ifreq *);
DEFINE_FACTORY(iovec_ptr, const iovec *);
DEFINE_FACTORY(msghdr_ptr, msghdr *);
DEFINE_FACTORY(msghdr_ptr, const msghdr *);
DEFINE_FACTORY(message_args_ptr, message_args *);
DEFINE_FACTORY(siginfo_t_ptr, siginfo_t *);
DEFINE_FACTORY(sockaddr_ptr, sockaddr *);
DEFINE_FACTORY(sockaddr_ptr, const sockaddr *);
DEFINE_FACTORY(sockaddr_args_ptr, sockaddr_args *);
DEFINE_FACTORY(socket_args_ptr, socket_args *);
DEFINE_FACTORY(sockopt_args_ptr, sockopt_args *);

DEFINE_FACTORY(rlimit_ptr, rlimit *);
DEFINE_FACTORY(rlimit_ptr, const rlimit *);

DEFINE_FACTORY(fdset_ptr, fd_set *);
DEFINE_FACTORY(pollfd_ptr, pollfd *);
DEFINE_FACTORY(object_wait_infos_ptr, object_wait_info *);
DEFINE_FACTORY(event_wait_infos_ptr, event_wait_info *);

DEFINE_FACTORY(int_ptr, int *);
DEFINE_FACTORY(long_ptr, long *);
DEFINE_FACTORY(longlong_ptr, long long *);
DEFINE_FACTORY(uint_ptr, unsigned int *);
DEFINE_FACTORY(ulong_ptr, unsigned long *);
DEFINE_FACTORY(ulonglong_ptr, unsigned long long *);

template<>
struct TypeHandlerFactory<void**> {
	static inline TypeHandler *Create()
	{
		return TypeHandlerFactory<addr_t*>::Create();
	}
};

// partial specialization for generic pointers
template<typename Type>
struct TypeHandlerFactory<Type*> {
	static inline TypeHandler *Create()
	{
		return create_pointer_type_handler();
	}
};

template<typename Type>
class TypeHandlerImpl : public TypeHandler {
public:
	string GetParameterValue(Context &, Parameter *, const void *);
	string GetReturnValue(Context &, uint64 value);
};

#endif	// STRACE_TYPE_HANDLER_H
