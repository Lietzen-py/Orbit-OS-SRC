/*
 * Copyright 2008-2011, Ingo Weinhold, ingo_weinhold@gmx.de.
 * Copyright 2004-2007, Axel Dörfler, axeld@pinc-software.de.
 * Distributed under the terms of the MIT License.
 *
 * Copyright 2001-2002, Travis Geiselbrecht. All rights reserved.
 * Distributed under the terms of the NewOS License.
 */
#ifndef _KERNEL_VM_STORE_ANONYMOUS_NO_SWAP_H
#define _KERNEL_VM_STORE_ANONYMOUS_NO_SWAP_H


#include <vm/VMCache.h>


class VMAnonymousNoSwapCache : public VMCache {
public:
	virtual						~VMAnonymousNoSwapCache();

			status_t			Init(bool canOvercommit,
									int32 numPrecommittedPages,
									int32 numGuardPages,
									uint32 allocationFlags);

	virtual	ssize_t				Discard(off_t offset, off_t size);

	virtual	bool				CanOvercommit();
	virtual	status_t			Commit(off_t size, int priority);
	virtual	bool				StoreHasPage(off_t offset);

	virtual	int32				GuardSize()	{ return fGuardedSize; }
	virtual	void				SetGuardSize(int32 guardSize)
									{ fGuardedSize = guardSize; }

	virtual	status_t			Read(off_t offset, const generic_io_vec *vecs,
									size_t count,uint32 flags,
									generic_size_t *_numBytes);
	virtual	status_t			Write(off_t offset, const generic_io_vec *vecs,
									size_t count, uint32 flags,
									generic_size_t *_numBytes);

	virtual	status_t			Fault(struct VMAddressSpace* aspace,
									off_t offset);

	virtual	void				Merge(VMCache* source);

protected:
	virtual	void				DeleteObject();

private:
			bool				fCanOvercommit;
			bool				fHasPrecommitted;
			uint8				fPrecommittedPages;
			int32				fGuardedSize;
};


#endif	/* _KERNEL_VM_STORE_ANONYMOUS_NO_SWAP_H */
