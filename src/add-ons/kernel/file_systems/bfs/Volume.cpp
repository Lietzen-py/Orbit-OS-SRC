/*
 * Copyright 2001-2019, Axel Dörfler, axeld@pinc-software.de.
 * This file may be used under the terms of the MIT License.
 */


//! superblock, mounting, etc.


#include "Attribute.h"
#include "CheckVisitor.h"
#include "Debug.h"
#include "file_systems/DeviceOpener.h"
#include "Inode.h"
#include "Journal.h"
#include "Query.h"
#include "Volume.h"


static const int32 kDesiredAllocationGroups = 56;
	// This is the number of allocation groups that will be tried
	// to be given for newly initialized disks.
	// That's only relevant for smaller disks, though, since any
	// of today's disk sizes already reach the maximum length
	// of an allocation group (65536 blocks).
	// It seems to create appropriate numbers for smaller disks
	// with this setting, though (i.e. you can create a 400 MB
	// file on a 1 GB disk without the need for double indirect
	// blocks).


//	#pragma mark -


bool
disk_super_block::IsMagicValid() const
{
	return Magic1() == (int32)SUPER_BLOCK_MAGIC1
		&& Magic2() == (int32)SUPER_BLOCK_MAGIC2
		&& Magic3() == (int32)SUPER_BLOCK_MAGIC3;
}


bool
disk_super_block::IsValid() const
{
	if (!IsMagicValid()
		|| (int32)block_size != inode_size
		|| ByteOrder() != SUPER_BLOCK_FS_LENDIAN
		|| (1UL << BlockShift()) != BlockSize()
		|| AllocationGroups() < 1
		|| AllocationGroupShift() < 1
		|| BlocksPerAllocationGroup() < 1
		|| NumBlocks() < 10
		|| AllocationGroups() != divide_roundup(NumBlocks(),
			1L << AllocationGroupShift()))
		return false;

	return true;
}


void
disk_super_block::Initialize(const char* diskName, off_t numBlocks,
	uint32 blockSize)
{
	memset(this, 0, sizeof(disk_super_block));

	magic1 = HOST_ENDIAN_TO_BFS_INT32(SUPER_BLOCK_MAGIC1);
	magic2 = HOST_ENDIAN_TO_BFS_INT32(SUPER_BLOCK_MAGIC2);
	magic3 = HOST_ENDIAN_TO_BFS_INT32(SUPER_BLOCK_MAGIC3);
	fs_byte_order = HOST_ENDIAN_TO_BFS_INT32(SUPER_BLOCK_FS_LENDIAN);
	flags = HOST_ENDIAN_TO_BFS_INT32(SUPER_BLOCK_DISK_CLEAN);

	strlcpy(name, diskName, sizeof(name));

	int32 blockShift = 9;
	while ((1UL << blockShift) < blockSize) {
		blockShift++;
	}

	block_size = inode_size = HOST_ENDIAN_TO_BFS_INT32(blockSize);
	block_shift = HOST_ENDIAN_TO_BFS_INT32(blockShift);

	num_blocks = HOST_ENDIAN_TO_BFS_INT64(numBlocks);
	used_blocks = 0;

	// Get the minimum ag_shift (that's determined by the block size)

	int32 bitsPerBlock = blockSize << 3;
	off_t bitmapBlocks = (numBlocks + bitsPerBlock - 1) / bitsPerBlock;
	int32 bitmapBlocksPerGroup = 1;
	int32 groupShift = 13;

	for (int32 i = 8192; i < bitsPerBlock; i *= 2) {
		groupShift++;
	}

	// Many allocation groups help applying allocation policies, but if
	// they are too small, we will need to many block_runs to cover large
	// files (see above to get an explanation of the kDesiredAllocationGroups
	// constant).

	int32 numGroups;

	while (true) {
		numGroups = (bitmapBlocks + bitmapBlocksPerGroup - 1) / bitmapBlocksPerGroup;
		if (numGroups > kDesiredAllocationGroups) {
			if (groupShift == 16)
				break;

			groupShift++;
			bitmapBlocksPerGroup *= 2;
		} else
			break;
	}

	num_ags = HOST_ENDIAN_TO_BFS_INT32(numGroups);
	// blocks_per_ag holds the number of bitmap blocks that are in each allocation group
	blocks_per_ag = HOST_ENDIAN_TO_BFS_INT32(bitmapBlocksPerGroup);
	ag_shift = HOST_ENDIAN_TO_BFS_INT32(groupShift);
}


//	#pragma mark -


Volume::Volume(fs_volume* volume)
	:
	fVolume(volume),
	fBlockAllocator(this),
	fRootNode(NULL),
	fIndicesNode(NULL),
	fDirtyCachedBlocks(0),
	fFlags(0),
	fCheckingThread(-1),
	fCheckVisitor(NULL)
{
	mutex_init(&fLock, "bfs volume");
	mutex_init(&fQueryLock, "bfs queries");
}


Volume::~Volume()
{
	mutex_destroy(&fQueryLock);
	mutex_destroy(&fLock);
}


bool
Volume::IsValidSuperBlock() const
{
	return fSuperBlock.IsValid();
}


/*!	Checks whether the given block number may be the location of an inode block.
*/
bool
Volume::IsValidInodeBlock(off_t block) const
{
	return block > fSuperBlock.LogEnd() && block < NumBlocks();
}


void
Volume::Panic()
{
	FATAL(("Disk corrupted... switch to read-only mode!\n"));
	fFlags |= VOLUME_READ_ONLY;
#if KDEBUG
	kernel_debugger("BFS panics!");
#endif
}


status_t
Volume::Mount(const char* deviceName, uint32 flags)
{
	// TODO: validate the FS in write mode as well!
#if (B_HOST_IS_LENDIAN && defined(BFS_BIG_ENDIAN_ONLY)) \
	|| (B_HOST_IS_BENDIAN && defined(BFS_LITTLE_ENDIAN_ONLY))
	// in big endian mode, we only mount read-only for now
	flags |= B_MOUNT_READ_ONLY;
#endif

	DeviceOpener opener(deviceName, (flags & B_MOUNT_READ_ONLY) != 0
		? O_RDONLY : O_RDWR);
	fDevice = opener.Device();
	if (fDevice < B_OK)
		RETURN_ERROR(fDevice);

	if (opener.IsReadOnly())
		fFlags |= VOLUME_READ_ONLY;

	// read the superblock
	if (Identify(fDevice, &fSuperBlock) != B_OK) {
		FATAL(("invalid superblock!\n"));
		return B_BAD_VALUE;
	}

	// initialize short hands to the superblock (to save byte swapping)
	fBlockSize = fSuperBlock.BlockSize();
	fBlockShift = fSuperBlock.BlockShift();
	fAllocationGroupShift = fSuperBlock.AllocationGroupShift();

	// check if the device size is large enough to hold the file system
	off_t diskSize;
	if (opener.GetSize(&diskSize, &fDeviceBlockSize) != B_OK)
		RETURN_ERROR(B_ERROR);
	if (diskSize < (NumBlocks() << BlockShift())) {
		FATAL(("Disk size (%" B_PRIdOFF " bytes) < file system size (%"
			B_PRIdOFF " bytes)!\n", diskSize, NumBlocks() << BlockShift()));
		RETURN_ERROR(B_BAD_VALUE);
	}

	// set the current log pointers, so that journaling will work correctly
	fLogStart = fSuperBlock.LogStart();
	fLogEnd = fSuperBlock.LogEnd();

	if ((fBlockCache = opener.InitCache(NumBlocks(), fBlockSize)) == NULL)
		return B_ERROR;

	fJournal = new(std::nothrow) Journal(this);
	if (fJournal == NULL)
		return B_NO_MEMORY;

	status_t status = fJournal->InitCheck();
	if (status < B_OK) {
		FATAL(("could not initialize journal: %s!\n", strerror(status)));
		return status;
	}

	// replaying the log is the first thing we will do on this disk
	status = fJournal->ReplayLog();
	if (status != B_OK) {
		FATAL(("Replaying log failed, data may be corrupted, volume "
			"read-only.\n"));
		fFlags |= VOLUME_READ_ONLY;
			// TODO: if this is the boot volume, Bootscript will assume this
			// is a CD...
			// TODO: it would be nice to have a user visible alert instead
			// of letting him just find this in the syslog.
	}

	status = fBlockAllocator.Initialize();
	if (status != B_OK) {
		FATAL(("could not initialize block bitmap allocator!\n"));
		return status;
	}

	fRootNode = new(std::nothrow) Inode(this, ToVnode(Root()));
	if (fRootNode != NULL && fRootNode->InitCheck() == B_OK) {
		status = publish_vnode(fVolume, ToVnode(Root()), (void*)fRootNode,
			&gBFSVnodeOps, fRootNode->Mode(), 0);
		if (status == B_OK) {
			// try to get indices root dir

			if (!Indices().IsZero()) {
				fIndicesNode = new(std::nothrow) Inode(this,
					ToVnode(Indices()));
			}

			if (fIndicesNode == NULL
				|| fIndicesNode->InitCheck() < B_OK
				|| !fIndicesNode->IsContainer()) {
				INFORM(("bfs: volume doesn't have indices!\n"));

				if (fIndicesNode) {
					// if this is the case, the index root node is gone bad,
					// and BFS switch to read-only mode
					fFlags |= VOLUME_READ_ONLY;
					delete fIndicesNode;
					fIndicesNode = NULL;
				}
			} else {
				// we don't use the vnode layer to access the indices node
			}
		} else {
			FATAL(("could not create root node: publish_vnode() failed!\n"));
			delete fRootNode;
			return status;
		}
	} else {
		status = B_BAD_VALUE;
		FATAL(("could not create root node!\n"));

		// We need to wait for the block allocator to finish
		fBlockAllocator.Uninitialize();
		return status;
	}

	// all went fine
	opener.Keep();
	return B_OK;
}


status_t
Volume::Unmount()
{
	put_vnode(fVolume, ToVnode(Root()));

	fBlockAllocator.Uninitialize();

	// This will also flush the log & all blocks to disk
	delete fJournal;
	fJournal = NULL;

	delete fIndicesNode;

	block_cache_delete(fBlockCache, !IsReadOnly());
	close(fDevice);

	return B_OK;
}


status_t
Volume::Sync()
{
	return fJournal->FlushLogAndBlocks();
}


status_t
Volume::ValidateBlockRun(block_run run)
{
	if (run.AllocationGroup() < 0
		|| run.AllocationGroup() > (int32)AllocationGroups()
		|| run.Start() > (1UL << AllocationGroupShift())
		|| run.length == 0
		|| uint32(run.Length() + run.Start())
				> (1UL << AllocationGroupShift())) {
		Panic();
		FATAL(("*** invalid run(%d,%d,%d)\n", (int)run.AllocationGroup(),
			run.Start(), run.Length()));
		return B_BAD_DATA;
	}
	return B_OK;
}


block_run
Volume::ToBlockRun(off_t block) const
{
	block_run run;
	run.allocation_group = HOST_ENDIAN_TO_BFS_INT32(
		block >> AllocationGroupShift());
	run.start = HOST_ENDIAN_TO_BFS_INT16(
		block & ((1LL << AllocationGroupShift()) - 1));
	run.length = HOST_ENDIAN_TO_BFS_INT16(1);
	return run;
}


status_t
Volume::CreateIndicesRoot(Transaction& transaction)
{
	off_t id;
	status_t status = Inode::Create(transaction, NULL, NULL,
		S_INDEX_DIR | S_STR_INDEX | S_DIRECTORY | 0700, 0, 0, NULL, &id,
		&fIndicesNode, NULL, BFS_DO_NOT_PUBLISH_VNODE);
	if (status < B_OK)
		RETURN_ERROR(status);

	fSuperBlock.indices = ToBlockRun(id);
	return WriteSuperBlock();
}


status_t
Volume::CreateVolumeID(Transaction& transaction)
{
	Attribute attr(fRootNode);
	status_t status;
	attr_cookie* cookie;
	status = attr.Create("be:volume_id", B_UINT64_TYPE, O_RDWR, &cookie);
	if (status == B_OK) {
		static bool seeded = false;
		if (!seeded) {
			// seed the random number generator for the be:volume_id attribute.
			srand(time(NULL));
			seeded = true;
		}
		uint64_t id;
		size_t length = sizeof(id);
		id = ((uint64_t)rand() << 32) | rand();
		attr.Write(transaction, cookie, 0, (uint8_t *)&id, &length, NULL);
	}
	return status;
}



status_t
Volume::AllocateForInode(Transaction& transaction, const Inode* parent,
	mode_t type, block_run& run)
{
	return fBlockAllocator.AllocateForInode(transaction, &parent->BlockRun(),
		type, run);
}


status_t
Volume::WriteSuperBlock()
{
	if (write_pos(fDevice, 512, &fSuperBlock, sizeof(disk_super_block))
			!= sizeof(disk_super_block))
		return B_IO_ERROR;

	return B_OK;
}


void
Volume::UpdateLiveQueries(Inode* inode, const char* attribute, int32 type,
	const uint8* oldKey, size_t oldLength, const uint8* newKey,
	size_t newLength)
{
	MutexLocker _(fQueryLock);

	DoublyLinkedList<Query>::Iterator iterator = fQueries.GetIterator();
	while (iterator.HasNext()) {
		Query* query = iterator.Next();
		query->LiveUpdate(inode, attribute, type, oldKey, oldLength, newKey,
			newLength);
	}
}


void
Volume::UpdateLiveQueriesRenameMove(Inode* inode, ino_t oldDirectoryID,
	const char* oldName, ino_t newDirectoryID, const char* newName)
{
	MutexLocker _(fQueryLock);

	size_t oldLength = strlen(oldName);
	size_t newLength = strlen(newName);

	DoublyLinkedList<Query>::Iterator iterator = fQueries.GetIterator();
	while (iterator.HasNext()) {
		Query* query = iterator.Next();
		query->LiveUpdateRenameMove(inode, oldDirectoryID, oldName, oldLength,
			newDirectoryID, newName, newLength);
	}
}


/*!	Checks if there is a live query whose results depend on the presence
	or value of the specified attribute.
	Don't use it if you already have all the data together to evaluate
	the queries - it wouldn't safe you anything in this case.
*/
bool
Volume::CheckForLiveQuery(const char* attribute)
{
	// TODO: check for a live query that depends on the specified attribute
	return true;
}


void
Volume::AddQuery(Query* query)
{
	MutexLocker _(fQueryLock);
	fQueries.Add(query);
}


void
Volume::RemoveQuery(Query* query)
{
	MutexLocker _(fQueryLock);
	fQueries.Remove(query);
}


status_t
Volume::CreateCheckVisitor()
{
	if (fCheckVisitor != NULL)
		return B_BUSY;

	fCheckVisitor = new(std::nothrow) ::CheckVisitor(this);
	if (fCheckVisitor == NULL)
		return B_NO_MEMORY;

	return B_OK;
}


void
Volume::DeleteCheckVisitor()
{
	delete fCheckVisitor;
	fCheckVisitor = NULL;
}


//	#pragma mark - Disk scanning and initialization


/*static*/ status_t
Volume::CheckSuperBlock(const uint8* data, uint32* _offset)
{
	disk_super_block* superBlock = (disk_super_block*)(data + 512);
	if (superBlock->IsMagicValid()) {
		if (superBlock->IsValid()) {
			if (_offset != NULL)
				*_offset = 512;
			return B_OK;
		}

		FATAL(("invalid superblock at offset 512!\n"));
	}

#ifndef BFS_LITTLE_ENDIAN_ONLY
	// For PPC, the superblock might be located at offset 0
	superBlock = (disk_super_block*)data;
	if (superBlock->IsValid()) {
		if (_offset != NULL)
			*_offset = 0;
		return B_OK;
	}
#endif

	return B_BAD_VALUE;
}


/*static*/ status_t
Volume::Identify(int fd, disk_super_block* superBlock)
{
	uint8 buffer[1024];
	if (read_pos(fd, 0, buffer, sizeof(buffer)) != sizeof(buffer))
		return B_IO_ERROR;

	uint32 offset;
	if (CheckSuperBlock(buffer, &offset) != B_OK)
		return B_BAD_VALUE;

	memcpy(superBlock, buffer + offset, sizeof(disk_super_block));
	return B_OK;
}


status_t
Volume::Initialize(int fd, const char* name, uint32 blockSize,
	uint32 flags)
{
	// although there is no really good reason for it, we won't
	// accept '/' in disk names (mkbfs does this, too - and since
	// Tracker names mounted volumes like their name)
	if (strchr(name, '/') != NULL)
		return B_BAD_VALUE;

	if (blockSize != 1024 && blockSize != 2048 && blockSize != 4096
		&& blockSize != 8192)
		return B_BAD_VALUE;

	DeviceOpener opener(fd, O_RDWR);
	if (opener.Device() < B_OK)
		return B_BAD_VALUE;

	if (opener.IsReadOnly())
		return B_READ_ONLY_DEVICE;

	fDevice = opener.Device();

	uint32 deviceBlockSize;
	off_t deviceSize;
	if (opener.GetSize(&deviceSize, &deviceBlockSize) < B_OK)
		return B_ERROR;

	off_t numBlocks = deviceSize / blockSize;

	// create valid superblock

	fSuperBlock.Initialize(name, numBlocks, blockSize);

	// initialize short hands to the superblock (to save byte swapping)
	fBlockSize = fSuperBlock.BlockSize();
	fBlockShift = fSuperBlock.BlockShift();
	fAllocationGroupShift = fSuperBlock.AllocationGroupShift();

	// determine log size depending on the size of the volume
	off_t logSize = 2048;
	if (numBlocks <= 20480)
		logSize = 512;
	if (deviceSize > 1LL * 1024 * 1024 * 1024)
		logSize = 4096;

	// since the allocator has not been initialized yet, we
	// cannot use BlockAllocator::BitmapSize() here
	off_t bitmapBlocks = (numBlocks + blockSize * 8 - 1) / (blockSize * 8);

	fSuperBlock.log_blocks = ToBlockRun(bitmapBlocks + 1);
	fSuperBlock.log_blocks.length = HOST_ENDIAN_TO_BFS_INT16(logSize);
	fSuperBlock.log_start = fSuperBlock.log_end = HOST_ENDIAN_TO_BFS_INT64(
		ToBlock(Log()));

	// set the current log pointers, so that journaling will work correctly
	fLogStart = fSuperBlock.LogStart();
	fLogEnd = fSuperBlock.LogEnd();

	if (!IsValidSuperBlock())
		RETURN_ERROR(B_ERROR);

	if ((fBlockCache = opener.InitCache(NumBlocks(), fBlockSize)) == NULL)
		return B_ERROR;

	fJournal = new(std::nothrow) Journal(this);
	if (fJournal == NULL || fJournal->InitCheck() < B_OK)
		RETURN_ERROR(B_ERROR);

	// ready to write data to disk

	Transaction transaction(this, 0);

	if (fBlockAllocator.InitializeAndClearBitmap(transaction) < B_OK)
		RETURN_ERROR(B_ERROR);

	off_t id;
	status_t status = Inode::Create(transaction, NULL, NULL,
		S_DIRECTORY | 0755, 0, 0, NULL, &id, &fRootNode);
	if (status < B_OK)
		RETURN_ERROR(status);

	fSuperBlock.root_dir = ToBlockRun(id);

	if ((flags & VOLUME_NO_INDICES) == 0) {
		// The indices root directory will be created automatically
		// when the standard indices are created (or any other).
		Index index(this);
		status = index.Create(transaction, "name", B_STRING_TYPE);
		if (status < B_OK)
			return status;

		status = index.Create(transaction, "BEOS:APP_SIG", B_STRING_TYPE);
		if (status < B_OK)
			return status;

		status = index.Create(transaction, "last_modified", B_INT64_TYPE);
		if (status < B_OK)
			return status;

		status = index.Create(transaction, "size", B_INT64_TYPE);
		if (status < B_OK)
			return status;
	}

	status = CreateVolumeID(transaction);
	if (status < B_OK)
		return status;

	status = _EraseUnusedBootBlock();
	if (status < B_OK)
		return status;

	status = WriteSuperBlock();
	if (status < B_OK)
		return status;

	status = transaction.Done();
	if (status < B_OK)
		return status;

	Sync();
	opener.RemoveCache(true);
	return B_OK;
}


/*!	Erase the first boot block, as we don't use it and there
 *	might be leftovers from other file systems. This can cause
 *	confusion for identifying the partition if not erased.
 */
status_t
Volume::_EraseUnusedBootBlock()
{
	const int32 blockSize = 512;
	const char emptySector[blockSize] = { 0 };
	// Erase boot block if any
	if (write_pos(fDevice, 0, emptySector, blockSize) != blockSize)
		return B_IO_ERROR;
	// Erase ext2 superblock if any
	if (write_pos(fDevice, 1024, emptySector, blockSize) != blockSize)
		return B_IO_ERROR;

	return B_OK;
}
