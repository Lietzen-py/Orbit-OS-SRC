/*
 * Copyright 2005-2011, Ingo Weinhold, ingo_weinhold@gmx.de.
 * Copyright 2013, Rene Gollent, rene@gollent.com.
 * Copyright 2015, Axel Dörfler, axeld@pinc-software.de.
 * Distributed under the terms of the MIT License.
 */


#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <signal.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include <debugger.h>
#include <image.h>
#include <syscalls.h>

#include "debug_utils.h"

#include "signals.h"
#include "Context.h"
#include "MemoryReader.h"
#include "Syscall.h"
#include "TypeHandler.h"


using std::map;
using std::string;
using std::vector;


struct syscall_stats {
	bigtime_t	time;
	uint32		count;
};


extern void get_syscalls0(vector<Syscall*> &syscalls);
extern void get_syscalls1(vector<Syscall*> &syscalls);
extern void get_syscalls2(vector<Syscall*> &syscalls);
extern void get_syscalls3(vector<Syscall*> &syscalls);
extern void get_syscalls4(vector<Syscall*> &syscalls);
extern void get_syscalls5(vector<Syscall*> &syscalls);
extern void get_syscalls6(vector<Syscall*> &syscalls);
extern void get_syscalls7(vector<Syscall*> &syscalls);
extern void get_syscalls8(vector<Syscall*> &syscalls);
extern void get_syscalls9(vector<Syscall*> &syscalls);
extern void get_syscalls10(vector<Syscall*> &syscalls);
extern void get_syscalls11(vector<Syscall*> &syscalls);
extern void get_syscalls12(vector<Syscall*> &syscalls);
extern void get_syscalls13(vector<Syscall*> &syscalls);
extern void get_syscalls14(vector<Syscall*> &syscalls);
extern void get_syscalls15(vector<Syscall*> &syscalls);
extern void get_syscalls16(vector<Syscall*> &syscalls);
extern void get_syscalls17(vector<Syscall*> &syscalls);
extern void get_syscalls18(vector<Syscall*> &syscalls);
extern void get_syscalls19(vector<Syscall*> &syscalls);


extern const char *__progname;
static const char *kCommandName = __progname;


// usage
static const char *kUsage =
"Usage: %s [ <options> ] [ <thread or team ID> | <executable with args> ]\n"
"\n"
"Traces the syscalls of a thread or a team. If an executable with\n"
"arguments is supplied, it is loaded and it's main thread traced.\n"
"\n"
"Options:\n"
"  -a             - Don't print syscall arguments.\n"
"  -c             - Record and dump syscall usage statistics.\n"
"  -C             - Same as -c, but also print syscalls as usual.\n"
"  -d <name>      - Filter the types that have their contents retrieved.\n"
"                   <name> is one of: strings, enums, simple, complex or\n"
"                                     pointer_values\n"
"  -e <names>     - Filter the syscalls.\n"
"                   <names> is a comma-separated list of names which can be:\n"
"                       * a syscall name\n"
"                       * %%memory for memory mapping related syscalls\n"
"                       * %%network or %%net for network related syscalls\n"
"  -f             - Fast mode. Syscall arguments contents aren't retrieved.\n"
"  -h, --help     - Print this text.\n"
"  -i             - Print integers in decimal format instead of hexadecimal.\n"
"  -l             - Also trace loading the executable. Only considered when\n"
"                   an executable is provided.\n"
"  --no-color     - Don't colorize output.\n"
"  -r             - Don't print syscall return values.\n"
"  -s             - Also trace all threads spawned by the supplied thread,\n"
"                   respectively the loaded executable's main thread.\n"
"  -t             - Also recursively trace all teams created by a traced\n"
"                   thread or team.\n"
"  -T             - Trace all threads of the supplied or loaded executable's\n"
"                   team. If an ID is supplied, it is interpreted as a team\n"
"                   ID.\n"
"  -o <file>      - directs output into the specified file.\n"
"  -S             - prints output to serial debug line.\n"
"  -g             - turns off signal tracing.\n"
;


// terminal color escape sequences
// (http://www.dee.ufcg.edu.br/~rrbrandt/tools/ansi.html)
static const char *kTerminalTextNormal	= "\33[0m";
static const char *kTerminalTextRed		= "\33[31m";
static const char *kTerminalTextMagenta	= "\33[35m";
static const char *kTerminalTextBlue	= "\33[34m";


// command line args
static int sArgc;
static const char *const *sArgv;

// syscalls
static vector<Syscall*>			sSyscallVector;
static map<string, Syscall*>	sSyscallMap;

// statistics
typedef map<string, syscall_stats> StatsMap;
static StatsMap sSyscallStats;
static bigtime_t sSyscallTime;


struct Team {
	Team(team_id id)
		:
		fID(id),
		fNubPort(-1)
	{
	}

	team_id ID() const
	{
		return fID;
	}

	port_id NubPort() const
	{
		return fNubPort;
	}

	MemoryReader& GetMemoryReader()
	{
		return fMemoryReader;
	}

	status_t InstallDebugger(port_id debuggerPort, bool traceTeam,
		bool traceChildTeams, bool traceSignal)
	{
		fNubPort = install_team_debugger(fID, debuggerPort);
		if (fNubPort < 0) {
			fprintf(stderr, "%s: Failed to install team debugger: %s\n",
				kCommandName, strerror(fNubPort));
			return fNubPort;
		}

		// set team debugging flags
		int32 teamDebugFlags =
			(traceTeam ? B_TEAM_DEBUG_PRE_SYSCALL | B_TEAM_DEBUG_POST_SYSCALL : 0)
			| (traceChildTeams ? B_TEAM_DEBUG_TEAM_CREATION : 0)
			| (traceSignal ? B_TEAM_DEBUG_SIGNALS : 0);
		if (set_team_debugging_flags(fNubPort, teamDebugFlags) != B_OK)
			exit(1);

		return fMemoryReader.Init(fNubPort);
	}

private:
	team_id			fID;
	port_id			fNubPort;
	MemoryReader	fMemoryReader;
};


static void
print_usage(bool error)
{
	// print usage
	fprintf((error ? stderr : stdout), kUsage, kCommandName);
}


static void
print_usage_and_exit(bool error)
{
	print_usage(error);
	exit(error ? 1 : 0);
}


static bool
get_id(const char *str, int32 &id)
{
	int32 len = strlen(str);
	for (int32 i = 0; i < len; i++) {
		if (!isdigit(str[i]))
			return false;
	}

	id = atol(str);
	return true;
}


Syscall *
get_syscall(const char *name)
{
	map<string, Syscall *>::const_iterator i = sSyscallMap.find(name);
	if (i == sSyscallMap.end())
		return NULL;

	return i->second;
}


static void
patch_syscalls()
{
	// instead of having this done here manually we should either add the
	// patching step to gensyscalls also manually or add metadata to
	// kernel/syscalls.h and have it parsed automatically

	extern void patch_area();
	extern void patch_events();
	extern void patch_exec();
	extern void patch_fcntl();
	extern void patch_ioctl();
	extern void patch_mutex();
	extern void patch_network();
	extern void patch_rlimit();

	for (size_t i = 0; i < sSyscallVector.size(); i++) {
		Syscall *syscall = sSyscallVector[i];

		// patch return type handlers
		const string returnTypeName = syscall->ReturnType()->TypeName();
		if (returnTypeName == "int" || returnTypeName == "status_t"
				|| returnTypeName == "area_id") {
			syscall->ReturnType()->SetHandler(create_status_t_type_handler());
		} else if (returnTypeName == "ssize_t") {
			syscall->ReturnType()->SetHandler(create_ssize_t_type_handler());
		}
	}

	patch_area();
	patch_events();
	patch_exec();
	patch_fcntl();
	patch_ioctl();
	patch_mutex();
	patch_network();
	patch_rlimit();
}


static void
init_syscalls()
{
	// init the syscall vector
	get_syscalls0(sSyscallVector);
	get_syscalls1(sSyscallVector);
	get_syscalls2(sSyscallVector);
	get_syscalls3(sSyscallVector);
	get_syscalls4(sSyscallVector);
	get_syscalls5(sSyscallVector);
	get_syscalls6(sSyscallVector);
	get_syscalls7(sSyscallVector);
	get_syscalls8(sSyscallVector);
	get_syscalls9(sSyscallVector);
	get_syscalls10(sSyscallVector);
	get_syscalls11(sSyscallVector);
	get_syscalls12(sSyscallVector);
	get_syscalls13(sSyscallVector);
	get_syscalls14(sSyscallVector);
	get_syscalls15(sSyscallVector);
	get_syscalls16(sSyscallVector);
	get_syscalls17(sSyscallVector);
	get_syscalls18(sSyscallVector);
	get_syscalls19(sSyscallVector);

	// init the syscall map
	int32 count = sSyscallVector.size();
	for (int32 i = 0; i < count; i++) {
		Syscall *syscall = sSyscallVector[i];
		sSyscallMap[syscall->Name()] = syscall;
	}

	patch_syscalls();
}


static void
record_syscall_stats(const Syscall& syscall, debug_post_syscall& message)
{
	syscall_stats& stats = sSyscallStats[syscall.Name()];
	stats.count++;

	if (message.start_time == 0)
		return;

	bigtime_t time = message.end_time - message.start_time;
	stats.time += time;
	sSyscallTime += time;
}


static void
print_buffer(FILE *outputFile, char* buffer, int32 length)
{
	// output either to file or serial debug line
	if (outputFile != NULL)
		fwrite(buffer, length, 1, outputFile);
	else
		_kern_debug_output(buffer);
}


static void
print_to_string(char **_buffer, int32 *_length, const char *format, ...)
{
	va_list list;
	va_start(list, format);
	ssize_t length = vsnprintf(*_buffer, *_length, format, list);
	va_end(list);

	if (length > *_length)
		length = *_length;

	*_buffer += length;
	*_length -= length;
}


static void
print_syscall(FILE *outputFile, Syscall* syscall, debug_pre_syscall &message,
	MemoryReader &memoryReader, bool printArguments, uint32 contentsFlags,
	bool colorize, bool decimal, thread_id &currentThreadID)
{
	char buffer[4096], *string = buffer;
	int32 length = (int32)sizeof(buffer);

	Context ctx(syscall, (char *)message.args, memoryReader,
		    contentsFlags | Context::INPUT_VALUES, decimal);

	if (currentThreadID != message.origin.thread) {
		if (currentThreadID != -1)
			print_to_string(&string, &length, " <unfinished ...>\n");
		currentThreadID = message.origin.thread;
	}

	// print syscall name, without the "_kern_"
	if (colorize) {
		print_to_string(&string, &length, "[%6" B_PRId32 "] %s%s%s",
			message.origin.thread, kTerminalTextBlue,
			syscall->Name().c_str() + 6, kTerminalTextNormal);
	} else {
		print_to_string(&string, &length, "[%6" B_PRId32 "] %s",
			message.origin.thread, syscall->Name().c_str() + 6);
	}

	// print arguments
	if (printArguments) {
		print_to_string(&string, &length, "(");

		int32 count = syscall->CountParameters();
		for (int32 i = 0; i < count; i++) {
			// get the value
			Parameter *parameter = syscall->ParameterAt(i);
			if (parameter->Out())
				continue;
			TypeHandler *handler = parameter->Handler();
			::string value =
				handler->GetParameterValue(ctx, parameter,
						ctx.GetValue(parameter));

			print_to_string(&string, &length, (i > 0 ? ", %s" : "%s"),
				value.c_str());
		}

		print_to_string(&string, &length, ")");
	}

	print_buffer(outputFile, buffer, sizeof(buffer) - length);
}


static void
print_syscall(FILE *outputFile, Syscall* syscall, debug_post_syscall &message,
	MemoryReader &memoryReader, bool printArguments, uint32 contentsFlags,
	bool printReturnValue, bool colorize, bool decimal,
	thread_id &currentThreadID)
{
	char buffer[4096], *string = buffer;
	int32 length = (int32)sizeof(buffer);
	bool threadChanged = false;

	Context ctx(syscall, (char *)message.args, memoryReader,
		    contentsFlags | Context::OUTPUT_VALUES, decimal, message.return_value);

	if (currentThreadID != message.origin.thread) {
		if (currentThreadID != -1) {
			print_to_string(&string, &length, " <unfinished ...>\n");
		}
		threadChanged = true;
	}
	currentThreadID = -1;

	// print return value
	if (printReturnValue) {
		if (threadChanged) {
			// print syscall name, without the "_kern_"
			if (colorize) {
				print_to_string(&string, &length, "[%6" B_PRId32 "] <... "
					"%s%s%s resumed> ", message.origin.thread, kTerminalTextBlue,
					syscall->Name().c_str() + 6, kTerminalTextNormal);
			} else {
				print_to_string(&string, &length, "[%6" B_PRId32 "] <... %s"
					" resumed> ", message.origin.thread,
					syscall->Name().c_str() + 6);
			}
		}

		Type *returnType = syscall->ReturnType();
		TypeHandler *handler = returnType->Handler();
		::string value = handler->GetReturnValue(ctx, message.return_value);
		if (value.length() > 0)
			print_to_string(&string, &length, " = %s", value.c_str());
	}

	// print arguments
	if (printArguments) {
		int32 count = syscall->CountParameters();
		int added = 0;
		bool printedParen = false;
		for (int32 i = 0; i < count; i++) {
			// get the value
			Parameter *parameter = syscall->ParameterAt(i);
			if (!parameter->InOut() && !parameter->Out())
				continue;

			TypeHandler *handler = parameter->Handler();
			::string value =
				handler->GetParameterValue(ctx, parameter,
					ctx.GetValue(parameter));

			if (!printedParen) {
				print_to_string(&string, &length, " (");
				printedParen = true;
			}
			print_to_string(&string, &length, (added > 0 ? ", %s" : "%s"),
				value.c_str());
			added++;
		}
		if (printedParen)
			print_to_string(&string, &length, ")");
	}

	bigtime_t duration = 0;
	if (message.start_time != 0)
		duration = message.end_time - message.start_time;
	if (colorize) {
		print_to_string(&string, &length, " %s(%lld us)%s\n", kTerminalTextMagenta,
			duration, kTerminalTextNormal);
	} else {
		print_to_string(&string, &length, " (%lld us)\n", duration);
	}

//for (int32 i = 0; i < 16; i++) {
//	if (i % 4 == 0) {
//		if (i > 0)
//			printf("\n");
//		printf("  ");
//	} else
//		printf(" ");
//	printf("%08lx", message.args[i]);
//}
//printf("\n");
	print_buffer(outputFile, buffer, sizeof(buffer) - length);
}


static void
print_signal(FILE *outputFile, debug_signal_received &message,
	bool colorize)
{
	char buffer[4096], *string = buffer;
	int32 length = (int32)sizeof(buffer);
	int signalNumber = message.signal;

	// print signal name
	if (colorize) {
		print_to_string(&string, &length, "[%6" B_PRId32 "] --- %s%s (%s)%s %s ---\n",
			message.origin.thread, kTerminalTextRed,
			signal_name(signalNumber).c_str(), strsignal(signalNumber),
			kTerminalTextNormal, signal_info(message.info).c_str());
	} else {
		print_to_string(&string, &length, "[%6" B_PRId32 "] --- %s (%s) %s ---\n",
			message.origin.thread, signal_name(signalNumber).c_str(),
			strsignal(signalNumber), signal_info(message.info).c_str());
	}

	print_buffer(outputFile, buffer, sizeof(buffer) - length);
}


static bool
compare_stats_by_time(
	const std::pair<const std::string*, const syscall_stats*>& a,
	const std::pair<const std::string*, const syscall_stats*>& b)
{
	return a.second->time > b.second->time;
}


static void
print_stats(FILE* outputFile)
{
	char buffer[4096], *string = buffer;
	int32 length = (int32)sizeof(buffer);

	typedef std::vector<std::pair<const std::string*, const syscall_stats*> >
		StatsRefVector;
	StatsRefVector calls;
	StatsMap::const_iterator iterator = sSyscallStats.begin();
	for (; iterator != sSyscallStats.end(); iterator++)
		calls.push_back(std::make_pair(&iterator->first, &iterator->second));

	// Sort calls by time spent
	std::sort(calls.begin(), calls.end(), compare_stats_by_time);

	print_to_string(&string, &length, "\n%-6s %-10s %-7s %-10s Syscall\n",
		"Time %", "Usecs", "Calls", "Usecs/call");
	print_to_string(&string, &length, "------ ---------- ------- ---------- "
		"--------------------\n");

	StatsRefVector::const_iterator callIterator = calls.begin();
	for (; callIterator != calls.end(); callIterator++) {
		const syscall_stats& stats = *callIterator->second;
		double percent = stats.time * 100.0 / sSyscallTime;
		bigtime_t perCall = stats.time / stats.count;

		print_to_string(&string, &length, "%6.2f %10" B_PRIu64 " %7" B_PRIu32
			" %10" B_PRIu64 " %s\n", percent, stats.time, stats.count, perCall,
			callIterator->first->c_str() + 6);
	}

	print_buffer(outputFile, buffer, sizeof(buffer) - length);
}


int
main(int argc, const char *const *argv)
{
	sArgc = argc;
	sArgv = argv;

	// parameters
	const char *const *programArgs = NULL;
	int32 programArgCount = 0;
	bool printArguments = true;
	bool colorize = true;
	bool stats = false;
	bool trace = true;
	uint32 contentsFlags = 0;
	bool decimalFormat = false;
	bool fastMode = false;
	bool traceLoading = false;
	bool printReturnValues = true;
	bool traceChildThreads = false;
	bool traceTeam = false;
	bool traceChildTeams = false;
	bool traceSignal = true;
	bool traceFilter = false;
	FILE *outputFile = stdout;

	// initialize our syscalls vector and map
	init_syscalls();

	// parse arguments
	for (int argi = 1; argi < argc; argi++) {
		const char *arg = argv[argi];
		if (arg[0] == '-') {
			// ToDo: improve option parsing so that ie. "-rsf" would also work
			if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
				print_usage_and_exit(false);
			} else if (strcmp(arg, "-a") == 0) {
				printArguments = false;
			} else if (strcmp(arg, "-c") == 0) {
				stats = true;
				trace = false;
			} else if (strcmp(arg, "-C") == 0) {
				stats = true;
			} else if (strcmp(arg, "--no-color") == 0) {
				colorize = false;
			} else if (strcmp(arg, "-d") == 0) {
				const char *what = NULL;

				if (arg[2] == '\0'
					&& argi + 1 < argc && argv[argi + 1][0] != '-') {
					// next arg is what
					what = argv[++argi];
				} else
					print_usage_and_exit(true);

				if (strcasecmp(what, "strings") == 0)
					contentsFlags |= Context::STRINGS;
				else if (strcasecmp(what, "enums") == 0)
					contentsFlags |= Context::ENUMERATIONS;
				else if (strcasecmp(what, "simple") == 0)
					contentsFlags |= Context::SIMPLE_STRUCTS;
				else if (strcasecmp(what, "complex") == 0)
					contentsFlags |= Context::COMPLEX_STRUCTS;
				else if (strcasecmp(what, "pointer_values") == 0)
					contentsFlags |= Context::POINTER_VALUES;
				else {
					fprintf(stderr, "%s: Unknown content filter `%s'\n",
						kCommandName, what);
					exit(1);
				}
			} else if (strcmp(arg, "-e") == 0) {
				traceFilter = true;
				// read filter string
				const char *filterString = NULL;
				if (arg[2] == '=') {
					// string follows
					filterString = arg + 3;
				} else if (arg[2] == '\0'
					&& argi + 1 < argc && argv[argi + 1][0] != '-') {
					// next arg is string
					filterString = argv[++argi];
				} else
					print_usage_and_exit(true);
				if (filterString != NULL) {
					char* copy = strdup(filterString);
					char *tok = strtok(copy, ",");
					while (tok != NULL) {
						if (tok[0] == '%') {
							tok++;
							// the following should be metadata in kernel/syscalls.h
							if (strcmp(tok, "memory") == 0) {
								sSyscallMap["_kern_clone_area"]->EnableTracing(true);
								sSyscallMap["_kern_create_area"]->EnableTracing(true);
								sSyscallMap["_kern_delete_area"]->EnableTracing(true);
								sSyscallMap["_kern_find_area"]->EnableTracing(true);
								sSyscallMap["_kern_resize_area"]->EnableTracing(true);
								sSyscallMap["_kern_transfer_area"]->EnableTracing(true);
								sSyscallMap["_kern_mlock"]->EnableTracing(true);
								sSyscallMap["_kern_munlock"]->EnableTracing(true);
								sSyscallMap["_kern_set_memory_protection"]->EnableTracing(true);
								sSyscallMap["_kern_get_memory_properties"]->EnableTracing(true);
								sSyscallMap["_kern_sync_memory"]->EnableTracing(true);
								sSyscallMap["_kern_unmap_memory"]->EnableTracing(true);
								sSyscallMap["_kern_memory_advice"]->EnableTracing(true);
								sSyscallMap["_kern_reserve_address_range"]->EnableTracing(true);
								sSyscallMap["_kern_unreserve_address_range"]->EnableTracing(true);
								sSyscallMap["_kern_set_area_protection"]->EnableTracing(true);
								sSyscallMap["_kern_map_file"]->EnableTracing(true);
							} else if (strcmp(tok, "network") == 0 || strcmp(tok, "net") == 0) {
								sSyscallMap["_kern_socket"]->EnableTracing(true);
								sSyscallMap["_kern_bind"]->EnableTracing(true);
								sSyscallMap["_kern_shutdown_socket"]->EnableTracing(true);
								sSyscallMap["_kern_connect"]->EnableTracing(true);
								sSyscallMap["_kern_listen"]->EnableTracing(true);
								sSyscallMap["_kern_accept"]->EnableTracing(true);
								sSyscallMap["_kern_recv"]->EnableTracing(true);
								sSyscallMap["_kern_recvfrom"]->EnableTracing(true);
								sSyscallMap["_kern_recvmsg"]->EnableTracing(true);
								sSyscallMap["_kern_send"]->EnableTracing(true);
								sSyscallMap["_kern_sendto"]->EnableTracing(true);
								sSyscallMap["_kern_sendmsg"]->EnableTracing(true);
								sSyscallMap["_kern_getsockopt"]->EnableTracing(true);
								sSyscallMap["_kern_setsockopt"]->EnableTracing(true);
								sSyscallMap["_kern_getpeername"]->EnableTracing(true);
								sSyscallMap["_kern_getsockname"]->EnableTracing(true);
								sSyscallMap["_kern_sockatmark"]->EnableTracing(true);
								sSyscallMap["_kern_socketpair"]->EnableTracing(true);
								sSyscallMap["_kern_get_next_socket_stat"]->EnableTracing(true);
							} else
								print_usage_and_exit(true);
						} else {
							char buffer[64];
							snprintf(buffer, sizeof(buffer), "_kern_%s", tok);
							Syscall* syscall = get_syscall(buffer);
							if (syscall == NULL)
								print_usage_and_exit(true);
							syscall->EnableTracing(true);
						}
					    tok = strtok(NULL, ",");
					}
					free(copy);
				}
			} else if (strcmp(arg, "-f") == 0) {
				fastMode = true;
			} else if (strcmp(arg, "-i") == 0) {
				decimalFormat = true;
			} else if (strcmp(arg, "-l") == 0) {
				traceLoading = true;
			} else if (strcmp(arg, "-r") == 0) {
				printReturnValues = false;
			} else if (strcmp(arg, "-s") == 0) {
				traceChildThreads = true;
			} else if (strcmp(arg, "-t") == 0) {
				traceChildTeams = true;
			} else if (strcmp(arg, "-T") == 0) {
				traceTeam = true;
			} else if (strcmp(arg, "-g") == 0) {
				traceSignal = false;
			} else if (strcmp(arg, "-S") == 0) {
				outputFile = NULL;
			} else if (strncmp(arg, "-o", 2) == 0) {
				// read filename
				const char *filename = NULL;
				if (arg[2] == '=') {
					// name follows
					filename = arg + 3;
				} else if (arg[2] == '\0'
					&& argi + 1 < argc && argv[argi + 1][0] != '-') {
					// next arg is name
					filename = argv[++argi];
				} else
					print_usage_and_exit(true);

				outputFile = fopen(filename, "w+");
				if (outputFile == NULL) {
					fprintf(stderr, "%s: Could not open `%s': %s\n",
						kCommandName, filename, strerror(errno));
					exit(1);
				}
			} else {
				print_usage_and_exit(true);
			}
		} else {
			programArgs = argv + argi;
			programArgCount = argc - argi;
			break;
		}
	}

	// check parameters
	if (!programArgs)
		print_usage_and_exit(true);

	if (fastMode)
		contentsFlags = 0;
	else if (contentsFlags == 0)
		contentsFlags = Context::ALL;

	// don't colorize the output, if we don't have a terminal
	if (outputFile == stdout)
		colorize = colorize && isatty(STDOUT_FILENO);
	else if (outputFile)
		colorize = false;

	// get thread/team to be debugged
	thread_id threadID = -1;
	team_id teamID = -1;
	if (programArgCount > 1
		|| !get_id(*programArgs, (traceTeam ? teamID : threadID))) {
		// we've been given an executable and need to load it
		threadID = load_program(programArgs, programArgCount, traceLoading);
		if (threadID < 0) {
			fprintf(stderr, "%s: Failed to start `%s': %s\n", kCommandName,
				programArgs[0], strerror(threadID));
			exit(1);
		}
	}

	// get the team ID, if we have none yet
	if (teamID < 0) {
		thread_info threadInfo;
		status_t error = get_thread_info(threadID, &threadInfo);
		if (error != B_OK) {
			fprintf(stderr, "%s: Failed to get info for thread %" B_PRId32
				": %s\n", kCommandName, threadID, strerror(error));
			exit(1);
		}
		teamID = threadInfo.team;
	}

	// create a debugger port
	port_id debuggerPort = create_port(10, "debugger port");
	if (debuggerPort < 0) {
		fprintf(stderr, "%s: Failed to create debugger port: %s\n",
			kCommandName, strerror(debuggerPort));
		exit(1);
	}

	// install ourselves as the team debugger
	typedef map<team_id, Team*> TeamMap;
	TeamMap debuggedTeams;
	port_id nubPort;

	{
		Team* team = new Team(teamID);
		status_t error = team->InstallDebugger(debuggerPort, traceTeam,
			traceChildTeams, traceSignal);
		if (error != B_OK)
			exit(1);

		debuggedTeams[team->ID()] = team;

		nubPort = team->NubPort();
	}

	// set thread debugging flags
	if (threadID >= 0) {
		int32 threadDebugFlags = 0;
		if (!traceTeam) {
			threadDebugFlags = B_THREAD_DEBUG_PRE_SYSCALL | B_THREAD_DEBUG_POST_SYSCALL
				| (traceChildThreads
					? B_THREAD_DEBUG_SYSCALL_TRACE_CHILD_THREADS : 0);
		}
		if (set_thread_debugging_flags(nubPort, threadID, threadDebugFlags)
				!= B_OK) {
			exit(1);
		}

		// resume the target thread to be sure it's running
		continue_thread(nubPort, threadID);
	}

	thread_id currentThreadID = -1;

	// debug loop
	while (true) {
		bool quitLoop = false;
		int32 code;
		debug_debugger_message_data message;
		ssize_t messageSize = read_port(debuggerPort, &code, &message,
			sizeof(message));

		if (messageSize < 0) {
			if (messageSize == B_INTERRUPTED)
				continue;

			fprintf(stderr, "%s: Reading from debugger port failed: %s\n",
				kCommandName, strerror(messageSize));
			exit(1);
		}

		switch (code) {
			case B_DEBUGGER_MESSAGE_PRE_SYSCALL:
			{
				TeamMap::iterator it = debuggedTeams.find(message.origin.team);
				if (it == debuggedTeams.end())
					break;

				Team* team = it->second;
				MemoryReader& memoryReader = team->GetMemoryReader();

				uint32 syscallNumber = message.pre_syscall.syscall;
				if (syscallNumber >= sSyscallVector.size()) {
					fprintf(stderr, "%s: invalid syscall %" B_PRIu32 " attempted\n",
						kCommandName, syscallNumber);
					break;
				}
				Syscall* syscall = sSyscallVector[syscallNumber];

				if (trace) {
					if (traceFilter && !syscall->TracingEnabled())
						break;
					print_syscall(outputFile, syscall, message.pre_syscall,
						memoryReader, printArguments, contentsFlags,
						colorize, decimalFormat, currentThreadID);
				}
				break;
			}

			case B_DEBUGGER_MESSAGE_POST_SYSCALL:
			{
				TeamMap::iterator it = debuggedTeams.find(message.origin.team);
				if (it == debuggedTeams.end())
					break;

				Team* team = it->second;
				MemoryReader& memoryReader = team->GetMemoryReader();

				uint32 syscallNumber = message.post_syscall.syscall;
				if (syscallNumber >= sSyscallVector.size()) {
					fprintf(stderr, "%s: invalid syscall %" B_PRIu32 " attempted\n",
						kCommandName, syscallNumber);
					break;
				}
				Syscall* syscall = sSyscallVector[syscallNumber];

				if (stats)
					record_syscall_stats(*syscall, message.post_syscall);

				if (trace) {
					if (traceFilter && !syscall->TracingEnabled())
						break;
					print_syscall(outputFile, syscall, message.post_syscall,
						memoryReader, printArguments, contentsFlags,
						printReturnValues, colorize, decimalFormat,
						currentThreadID);
				}
				break;
			}

			case B_DEBUGGER_MESSAGE_SIGNAL_RECEIVED:
			{
				if (traceSignal && trace)
					print_signal(outputFile, message.signal_received, colorize);
				break;
			}

			case B_DEBUGGER_MESSAGE_THREAD_DEBUGGED:
			case B_DEBUGGER_MESSAGE_DEBUGGER_CALL:
			case B_DEBUGGER_MESSAGE_BREAKPOINT_HIT:
			case B_DEBUGGER_MESSAGE_WATCHPOINT_HIT:
			case B_DEBUGGER_MESSAGE_SINGLE_STEP:
			case B_DEBUGGER_MESSAGE_EXCEPTION_OCCURRED:
			case B_DEBUGGER_MESSAGE_THREAD_CREATED:
			case B_DEBUGGER_MESSAGE_THREAD_DELETED:
			case B_DEBUGGER_MESSAGE_IMAGE_CREATED:
			case B_DEBUGGER_MESSAGE_IMAGE_DELETED:
				break;

			case B_DEBUGGER_MESSAGE_TEAM_CREATED:
			{
				if (!traceChildTeams)
					break;

				Team* team = new(std::nothrow) Team(
					message.team_created.new_team);
				if (team == NULL) {
					fprintf(stderr, "%s: Out of memory!\n", kCommandName);
					break;
				}

				status_t error = team->InstallDebugger(debuggerPort, true, true,
					traceSignal);
				if (error != B_OK) {
					delete team;
					break;
				}

				debuggedTeams[team->ID()] = team;
				break;
			}

			case B_DEBUGGER_MESSAGE_TEAM_DELETED:
			{
				// a debugged team is gone
				TeamMap::iterator it = debuggedTeams.find(message.origin.team);
				if (it == debuggedTeams.end())
					break;

				Team* team = it->second;
				debuggedTeams.erase(it);
				delete team;

				// if all debugged teams are gone, we're done
				quitLoop = debuggedTeams.empty();
				break;
			}
		}

		if (quitLoop)
			break;

		// tell the thread to continue (only when there is a thread and the
		// message was synchronous)
		if (message.origin.thread >= 0 && message.origin.nub_port >= 0) {
			if (continue_thread(message.origin.nub_port,
					message.origin.thread) != B_OK) {
				// the team can already be gone
			}
		}
	}

	if (stats) {
		// Dump recorded statistics
		print_stats(outputFile);
	}

	if (outputFile != NULL && outputFile != stdout)
		fclose(outputFile);

	return 0;
}
