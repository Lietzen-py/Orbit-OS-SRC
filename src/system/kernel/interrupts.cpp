/*
 * Copyright 2013, Paweł Dziepak, pdziepak@quarnos.org.
 * Copyright 2011, Michael Lotz, mmlr@mlotz.ch.
 * Copyright 2002-2010, Axel Dörfler, axeld@pinc-software.de.
 * Distributed under the terms of the MIT License.
 *
 * Copyright 2001-2002, Travis Geiselbrecht. All rights reserved.
 * Distributed under the terms of the NewOS License.
 */


#include <interrupts.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <arch/debug_console.h>
#include <arch/int.h>
#include <boot/kernel_args.h>
#include <elf.h>
#include <load_tracking.h>
#include <util/AutoLock.h>
#include <smp.h>

#include "kernel_debug_config.h"


//#define TRACE_INT
#ifdef TRACE_INT
#	define TRACE(x) dprintf x
#else
#	define TRACE(x) ;
#endif


struct io_handler {
	struct io_handler	*next;
	interrupt_handler	func;
	void				*data;
	bool				use_enable_counter;
	bool				no_handled_info;
#if DEBUG_INTERRUPTS
	int64				handled_count;
#endif
};

struct io_vector {
	struct io_handler	*handler_list;
	spinlock			vector_lock;
	int32				enable_count;
	bool				no_lock_vector;
	interrupt_type		type;

	spinlock			load_lock;
	bigtime_t			last_measure_time;
	bigtime_t			last_measure_active;
	int32				load;

	irq_assignment*		assigned_cpu;

#if DEBUG_INTERRUPTS
	int64				handled_count;
	int64				unhandled_count;
	int					trigger_count;
	int					ignored_count;
#endif
};

static int32 sLastCPU;

static io_vector sVectors[NUM_IO_VECTORS];
static bool sAllocatedIOInterruptVectors[NUM_IO_VECTORS];
static irq_assignment sVectorCPUAssignments[NUM_IO_VECTORS];
static mutex sIOInterruptVectorAllocationLock
	= MUTEX_INITIALIZER("io_interrupt_vector_allocation");


#if DEBUG_INTERRUPTS
static int
dump_int_statistics(int argc, char **argv)
{
	int i;
	for (i = 0; i < NUM_IO_VECTORS; i++) {
		struct io_handler *io;

		if (!B_SPINLOCK_IS_LOCKED(&sVectors[i].vector_lock)
			&& sVectors[i].enable_count == 0
			&& sVectors[i].handled_count == 0
			&& sVectors[i].unhandled_count == 0
			&& sVectors[i].handler_list == NULL)
			continue;

		kprintf("int %3d, enabled %" B_PRId32 ", handled %8" B_PRId64 ", "
			"unhandled %8" B_PRId64 "%s%s\n", i, sVectors[i].enable_count,
			sVectors[i].handled_count,sVectors[i].unhandled_count,
			B_SPINLOCK_IS_LOCKED(&sVectors[i].vector_lock) ? ", ACTIVE" : "",
			sVectors[i].handler_list == NULL ? ", no handler" : "");

		for (io = sVectors[i].handler_list; io != NULL; io = io->next) {
			const char *symbol, *imageName;
			bool exactMatch;

			status_t error = elf_debug_lookup_symbol_address((addr_t)io->func,
				NULL, &symbol, &imageName, &exactMatch);
			if (error == B_OK && exactMatch) {
				if (strchr(imageName, '/') != NULL)
					imageName = strrchr(imageName, '/') + 1;

				int length = 4 + strlen(imageName);
				kprintf("   %s:%-*s (%p)", imageName, 45 - length, symbol,
					io->func);
			} else
				kprintf("\t\t\t\t\t   func %p", io->func);

			kprintf(", data %p, handled ", io->data);
			if (io->no_handled_info)
				kprintf("<unknown>\n");
			else
				kprintf("%8" B_PRId64 "\n", io->handled_count);
		}

		kprintf("\n");
	}
	return 0;
}
#endif


static int
dump_int_load(int argc, char** argv)
{
	static const char* typeNames[]
		= { "exception", "irq", "local irq", "syscall", "ici", "unknown" };

	for (int i = 0; i < NUM_IO_VECTORS; i++) {
		if (!B_SPINLOCK_IS_LOCKED(&sVectors[i].vector_lock)
			&& sVectors[i].handler_list == NULL
			&& sVectors[i].enable_count == 0)
			continue;

		kprintf("int %3d, type %s, enabled %" B_PRId32 ", load %" B_PRId32
			"%%", i, typeNames[min_c(sVectors[i].type,
					INTERRUPT_TYPE_UNKNOWN)],
			sVectors[i].enable_count,
			sVectors[i].assigned_cpu != NULL
				? sVectors[i].assigned_cpu->load / 10 : 0);

		if (sVectors[i].type == INTERRUPT_TYPE_IRQ) {
			ASSERT(sVectors[i].assigned_cpu != NULL);

			if (sVectors[i].assigned_cpu->cpu != -1)
				kprintf(", cpu %" B_PRId32, sVectors[i].assigned_cpu->cpu);
			else
				kprintf(", cpu -");
		}

		if (B_SPINLOCK_IS_LOCKED(&sVectors[i].vector_lock))
			kprintf(", ACTIVE");
		kprintf("\n");
	}

	return 0;
}


//	#pragma mark - private kernel API


bool
interrupts_enabled(void)
{
	return arch_int_are_interrupts_enabled();
}


status_t
interrupts_init(kernel_args* args)
{
	TRACE(("interrupts_init: entry\n"));

	return arch_int_init(args);
}


status_t
interrupts_init_post_vm(kernel_args* args)
{
	int i;

	/* initialize the vector list */
	for (i = 0; i < NUM_IO_VECTORS; i++) {
		B_INITIALIZE_SPINLOCK(&sVectors[i].vector_lock);
		sVectors[i].enable_count = 0;
		sVectors[i].no_lock_vector = false;
		sVectors[i].type = INTERRUPT_TYPE_UNKNOWN;

		B_INITIALIZE_SPINLOCK(&sVectors[i].load_lock);
		sVectors[i].last_measure_time = 0;
		sVectors[i].last_measure_active = 0;
		sVectors[i].load = 0;

#if DEBUG_INTERRUPTS
		sVectors[i].handled_count = 0;
		sVectors[i].unhandled_count = 0;
		sVectors[i].trigger_count = 0;
		sVectors[i].ignored_count = 0;
#endif
		sVectors[i].handler_list = NULL;

		sVectorCPUAssignments[i].irq = i;
		sVectorCPUAssignments[i].count = 1;
		sVectorCPUAssignments[i].handlers_count = 0;
		sVectorCPUAssignments[i].load = 0;
		sVectorCPUAssignments[i].cpu = -1;
	}

#if DEBUG_INTERRUPTS
	add_debugger_command("ints", &dump_int_statistics,
		"list interrupt statistics");
#endif

	add_debugger_command("int_load", &dump_int_load,
		"list interrupt usage statistics");

	return arch_int_init_post_vm(args);
}


status_t
interrupts_init_io(kernel_args* args)
{
	return arch_int_init_io(args);
}


status_t
interrupts_init_post_device_manager(kernel_args* args)
{
	arch_debug_install_interrupt_handlers();

	return arch_int_init_post_device_manager(args);
}


static void
update_int_load(int i)
{
	if (!try_acquire_spinlock(&sVectors[i].load_lock))
		return;

	int32 oldLoad = sVectors[i].load;
	compute_load(sVectors[i].last_measure_time, sVectors[i].last_measure_active,
		sVectors[i].load, system_time());

	if (oldLoad != sVectors[i].load)
		atomic_add(&sVectors[i].assigned_cpu->load, sVectors[i].load - oldLoad);

	release_spinlock(&sVectors[i].load_lock);
}


/*!	Actually process an interrupt via the handlers registered for that
	vector (IRQ).
*/
int
io_interrupt_handler(int vector, bool levelTriggered)
{
	int status = B_UNHANDLED_INTERRUPT;
	struct io_handler* io;
	bool handled = false;

	bigtime_t start = system_time();

	// exceptions and syscalls have their own handlers
	ASSERT(sVectors[vector].type != INTERRUPT_TYPE_EXCEPTION
		&& sVectors[vector].type != INTERRUPT_TYPE_SYSCALL);

	if (!sVectors[vector].no_lock_vector)
		acquire_spinlock(&sVectors[vector].vector_lock);

#if !DEBUG_INTERRUPTS
	// The list can be empty at this place
	if (sVectors[vector].handler_list == NULL) {
		dprintf("unhandled io interrupt %d\n", vector);
		if (!sVectors[vector].no_lock_vector)
			release_spinlock(&sVectors[vector].vector_lock);
		return B_UNHANDLED_INTERRUPT;
	}
#endif

	// For level-triggered interrupts, we actually handle the return
	// value (ie. B_HANDLED_INTERRUPT) to decide whether or not we
	// want to call another interrupt handler.
	// For edge-triggered interrupts, however, we always need to call
	// all handlers, as multiple interrupts cannot be identified. We
	// still make sure the return code of this function will issue
	// whatever the driver thought would be useful.

	for (io = sVectors[vector].handler_list; io != NULL; io = io->next) {
		status = io->func(io->data);

#if DEBUG_INTERRUPTS
		if (status != B_UNHANDLED_INTERRUPT)
			io->handled_count++;
#endif
		if (levelTriggered && status != B_UNHANDLED_INTERRUPT)
			break;

		if (status == B_HANDLED_INTERRUPT || status == B_INVOKE_SCHEDULER)
			handled = true;
	}

	ASSERT_PRINT(!are_interrupts_enabled(),
		"interrupts enabled after calling handlers for vector %d", vector);

#if DEBUG_INTERRUPTS
	sVectors[vector].trigger_count++;
	if (status != B_UNHANDLED_INTERRUPT || handled) {
		sVectors[vector].handled_count++;
	} else {
		sVectors[vector].unhandled_count++;
		sVectors[vector].ignored_count++;
	}

	if (sVectors[vector].trigger_count > 10000) {
		if (sVectors[vector].ignored_count > 9900) {
			struct io_handler *last = sVectors[vector].handler_list;
			while (last && last->next)
				last = last->next;

			if (last != NULL && last->no_handled_info) {
				// we have an interrupt handler installed that does not
				// know whether or not it has actually handled the interrupt,
				// so this unhandled count is inaccurate and we can't just
				// disable
			} else {
				if (sVectors[vector].handler_list == NULL
					|| sVectors[vector].handler_list->next == NULL) {
					// this interrupt vector is not shared, disable it
					sVectors[vector].enable_count = -100;
					arch_int_disable_io_interrupt(vector);
					dprintf("Disabling unhandled io interrupt %d\n", vector);
				} else {
					// this is a shared interrupt vector, we cannot just disable it
					dprintf("More than 99%% interrupts of vector %d are unhandled\n",
						vector);
				}
			}
		}

		sVectors[vector].trigger_count = 0;
		sVectors[vector].ignored_count = 0;
	}
#endif

	if (!sVectors[vector].no_lock_vector)
		release_spinlock(&sVectors[vector].vector_lock);

	SpinLocker vectorLocker(sVectors[vector].load_lock);
	bigtime_t deltaTime = system_time() - start;
	sVectors[vector].last_measure_active += deltaTime;
	vectorLocker.Unlock();

	cpu_ent* cpu = get_cpu_struct();
	if (sVectors[vector].type == INTERRUPT_TYPE_IRQ
		|| sVectors[vector].type == INTERRUPT_TYPE_ICI
		|| sVectors[vector].type == INTERRUPT_TYPE_LOCAL_IRQ) {
		cpu->interrupt_time += deltaTime;
		if (sVectors[vector].type == INTERRUPT_TYPE_IRQ)
			cpu->irq_time += deltaTime;
	}

	update_int_load(vector);

	if (levelTriggered)
		return status;

	// edge triggered return value

	if (handled)
		return B_HANDLED_INTERRUPT;

	return B_UNHANDLED_INTERRUPT;
}


//	#pragma mark - public API


#undef disable_interrupts
#undef restore_interrupts


cpu_status
disable_interrupts(void)
{
	return arch_int_disable_interrupts();
}


void
restore_interrupts(cpu_status status)
{
	arch_int_restore_interrupts(status);
}


static
uint32 assign_cpu(void)
{
	const cpu_topology_node* node;
	do {
		int32 nextID = atomic_add(&sLastCPU, 1);
		node = get_cpu_topology();

		while (node->level != CPU_TOPOLOGY_SMT) {
			int levelSize = node->children_count;
			node = node->children[nextID % levelSize];
			nextID /= levelSize;
		}
	} while (gCPU[node->id].disabled);

	return node->id;
}


/*!	Install a handler to be called when an interrupt is triggered
	for the given interrupt number with \a data as the argument.
*/
status_t
install_io_interrupt_handler(int32 vector, interrupt_handler handler, void *data,
	uint32 flags)
{
	struct io_handler *io = NULL;
	cpu_status state;

	if (vector < 0 || vector >= NUM_IO_VECTORS)
		return B_BAD_VALUE;

	io = (struct io_handler *)malloc(sizeof(struct io_handler));
	if (io == NULL)
		return B_NO_MEMORY;

	arch_debug_remove_interrupt_handler(vector);
		// There might be a temporary debug interrupt installed on this
		// vector that should be removed now.

	io->func = handler;
	io->data = data;
	io->use_enable_counter = (flags & B_NO_ENABLE_COUNTER) == 0;
	io->no_handled_info = (flags & B_NO_HANDLED_INFO) != 0;
#if DEBUG_INTERRUPTS
	io->handled_count = 0LL;
#endif

	// Disable the interrupts, get the spinlock for this irq only
	// and then insert the handler
	state = disable_interrupts();
	acquire_spinlock(&sVectors[vector].vector_lock);

	// Initial attempt to balance IRQs, the scheduler will correct this
	// if some cores end up being overloaded.
	if (sVectors[vector].type == INTERRUPT_TYPE_IRQ
		&& sVectors[vector].handler_list == NULL
		&& sVectors[vector].assigned_cpu->cpu == -1) {

		int32 cpuID = assign_cpu();
		cpuID = arch_int_assign_to_cpu(vector, cpuID);
		sVectors[vector].assigned_cpu->cpu = cpuID;

		cpu_ent* cpu = &gCPU[cpuID];
		SpinLocker _(cpu->irqs_lock);
		atomic_add(&sVectors[vector].assigned_cpu->handlers_count, 1);
		list_add_item(&cpu->irqs, sVectors[vector].assigned_cpu);
	}

	if ((flags & B_NO_HANDLED_INFO) != 0
		&& sVectors[vector].handler_list != NULL) {
		// The driver registering this interrupt handler doesn't know
		// whether or not it actually handled the interrupt after the
		// handler returns. This is incompatible with shared interrupts
		// as we'd potentially steal interrupts from other handlers
		// resulting in interrupt storms. Therefore we enqueue this interrupt
		// handler as the very last one, meaning all other handlers will
		// get their go at any interrupt first.
		struct io_handler *last = sVectors[vector].handler_list;
		while (last->next)
			last = last->next;

		io->next = NULL;
		last->next = io;
	} else {
		// A normal interrupt handler, just add it to the head of the list.
		io->next = sVectors[vector].handler_list;
		sVectors[vector].handler_list = io;
	}

	// If B_NO_ENABLE_COUNTER is set, we're being asked to not alter
	// whether the interrupt should be enabled or not
	if (io->use_enable_counter) {
		if (sVectors[vector].enable_count++ == 0)
			arch_int_enable_io_interrupt(vector);
	}

	// If B_NO_LOCK_VECTOR is specified this is a vector that is not supposed
	// to have multiple handlers and does not require locking of the vector
	// when entering the handler. For example this is used by internally
	// registered interrupt handlers like for handling local APIC interrupts
	// that may run concurently on multiple CPUs. Locking with a spinlock
	// would in that case defeat the purpose as it would serialize calling the
	// handlers in parallel on different CPUs.
	if (flags & B_NO_LOCK_VECTOR)
		sVectors[vector].no_lock_vector = true;

	release_spinlock(&sVectors[vector].vector_lock);

	restore_interrupts(state);

	return B_OK;
}


/*!	Remove a previously installed interrupt handler */
status_t
remove_io_interrupt_handler(int32 vector, interrupt_handler handler, void *data)
{
	status_t status = B_BAD_VALUE;
	struct io_handler *io = NULL;
	struct io_handler *last = NULL;
	cpu_status state;

	if (vector < 0 || vector >= NUM_IO_VECTORS)
		return B_BAD_VALUE;

	/* lock the structures down so it is not modified while we search */
	state = disable_interrupts();
	acquire_spinlock(&sVectors[vector].vector_lock);

	/* loop through the available handlers and try to find a match.
	 * We go forward through the list but this means we start with the
	 * most recently added handlers.
	 */
	for (io = sVectors[vector].handler_list; io != NULL; io = io->next) {
		/* we have to match both function and data */
		if (io->func == handler && io->data == data) {
			if (last != NULL)
				last->next = io->next;
			else
				sVectors[vector].handler_list = io->next;

			// Check if we need to disable the interrupt
			if (io->use_enable_counter && --sVectors[vector].enable_count == 0)
				arch_int_disable_io_interrupt(vector);

			status = B_OK;
			break;
		}

		last = io;
	}

	if (sVectors[vector].handler_list == NULL
		&& sVectors[vector].type == INTERRUPT_TYPE_IRQ
		&& sVectors[vector].assigned_cpu != NULL
		&& sVectors[vector].assigned_cpu->handlers_count > 0) {

		int32 oldHandlersCount
			= atomic_add(&sVectors[vector].assigned_cpu->handlers_count, -1);

		if (oldHandlersCount == 1) {
			int32 oldCPU;
			SpinLocker locker;
			cpu_ent* cpu;

			do {
				locker.Unlock();

				oldCPU = sVectors[vector].assigned_cpu->cpu;

				ASSERT(oldCPU != -1);
				cpu = &gCPU[oldCPU];

				locker.SetTo(cpu->irqs_lock, false);
			} while (sVectors[vector].assigned_cpu->cpu != oldCPU);

			sVectors[vector].assigned_cpu->cpu = -1;
			list_remove_item(&cpu->irqs, sVectors[vector].assigned_cpu);
		}
	}

	release_spinlock(&sVectors[vector].vector_lock);
	restore_interrupts(state);

	// if the handler could be found and removed, we still have to free it
	if (status == B_OK)
		free(io);

	return status;
}


/*	Mark \a count contigous interrupts starting at \a startVector as in use.
	This will prevent them from being allocated by others. Only use this when
	the reserved range is hardwired to the given vector, otherwise allocate
	vectors using allocate_io_interrupt_vectors() instead.
*/
status_t
reserve_io_interrupt_vectors(int32 count, int32 startVector, interrupt_type type)
{
	MutexLocker locker(&sIOInterruptVectorAllocationLock);

	for (int32 i = 0; i < count; i++) {
		if (sAllocatedIOInterruptVectors[startVector + i]) {
			panic("reserved interrupt vector range %" B_PRId32 "-%" B_PRId32 " overlaps already "
				"allocated vector %" B_PRId32, startVector, startVector + count - 1,
				startVector + i);
			free_io_interrupt_vectors(i, startVector);
			return B_BUSY;
		}

		sVectors[startVector + i].type = type;
		sVectors[startVector + i].assigned_cpu
			= &sVectorCPUAssignments[startVector + i];
		sVectorCPUAssignments[startVector + i].count = 1;
		sAllocatedIOInterruptVectors[startVector + i] = true;
	}

	dprintf("reserve_io_interrupt_vectors: reserved %" B_PRId32 " vectors starting "
		"from %" B_PRId32 "\n", count, startVector);
	return B_OK;
}


/*!	Allocate \a count contiguous interrupt vectors. The vectors are allocated
	as available so that they do not overlap with any other reserved vector.
	The first vector to be used is returned in \a startVector on success.
*/
status_t
allocate_io_interrupt_vectors(int32 count, int32 *startVector,
	interrupt_type type)
{
	MutexLocker locker(&sIOInterruptVectorAllocationLock);

	int32 vector = 0;
	bool runFound = true;
	for (int32 i = 0; i < NUM_IO_VECTORS - (count - 1); i++) {
		if (sAllocatedIOInterruptVectors[i])
			continue;

		vector = i;
		runFound = true;
		for (uint16 j = 1; j < count; j++) {
			if (sAllocatedIOInterruptVectors[i + j]) {
				runFound = false;
				i += j;
				break;
			}
		}

		if (runFound)
			break;
	}

	if (!runFound) {
		dprintf("found no free vectors to allocate %" B_PRId32 " io interrupts\n", count);
		return B_NO_MEMORY;
	}

	for (int32 i = 0; i < count; i++) {
		sVectors[vector + i].type = type;
		sVectors[vector + i].assigned_cpu = &sVectorCPUAssignments[vector];
		sAllocatedIOInterruptVectors[vector + i] = true;
	}

	sVectorCPUAssignments[vector].irq = vector;
	sVectorCPUAssignments[vector].count = count;

	*startVector = vector;
	dprintf("allocate_io_interrupt_vectors: allocated %" B_PRId32 " vectors starting "
		"from %" B_PRId32 "\n", count, vector);
	return B_OK;
}


/*!	Free/unreserve interrupt vectors previously allocated with the
	{reserve|allocate}_io_interrupt_vectors() functions. The \a count and
	\a startVector can be adjusted from the allocation calls to partially free
	a vector range.
*/
void
free_io_interrupt_vectors(int32 count, int32 startVector)
{
	if (startVector + count > NUM_IO_VECTORS) {
		panic("invalid start vector %" B_PRId32 " or count %" B_PRId32 " supplied to "
			"free_io_interrupt_vectors\n", startVector, count);
		return;
	}

	dprintf("free_io_interrupt_vectors: freeing %" B_PRId32 " vectors starting "
		"from %" B_PRId32 "\n", count, startVector);

	MutexLocker locker(sIOInterruptVectorAllocationLock);
	for (int32 i = 0; i < count; i++) {
		if (!sAllocatedIOInterruptVectors[startVector + i]) {
			panic("io interrupt vector %" B_PRId32 " was not allocated\n",
				startVector + i);
		}

		io_vector& vector = sVectors[startVector + i];
		InterruptsSpinLocker vectorLocker(vector.vector_lock);
		if (vector.assigned_cpu != NULL && vector.assigned_cpu->cpu != -1) {
			panic("freeing io interrupt vector %" B_PRId32 " that is still asigned to a "
				"cpu", startVector + i);
			continue;
		}

		vector.assigned_cpu = NULL;
		sAllocatedIOInterruptVectors[startVector + i] = false;
	}
}


void
assign_io_interrupt_to_cpu(int32 vector, int32 newCPU)
{
	ASSERT(sVectors[vector].type == INTERRUPT_TYPE_IRQ);

	int32 oldCPU = sVectors[vector].assigned_cpu->cpu;

	if (newCPU == -1)
		newCPU = assign_cpu();

	if (newCPU == oldCPU)
		return;

	ASSERT(oldCPU != -1);
	cpu_ent* cpu = &gCPU[oldCPU];

	SpinLocker locker(cpu->irqs_lock);
	sVectors[vector].assigned_cpu->cpu = -1;
	list_remove_item(&cpu->irqs, sVectors[vector].assigned_cpu);
	locker.Unlock();

	newCPU = arch_int_assign_to_cpu(vector, newCPU);
	sVectors[vector].assigned_cpu->cpu = newCPU;
	cpu = &gCPU[newCPU];
	locker.SetTo(cpu->irqs_lock, false);
	list_add_item(&cpu->irqs, sVectors[vector].assigned_cpu);
}
