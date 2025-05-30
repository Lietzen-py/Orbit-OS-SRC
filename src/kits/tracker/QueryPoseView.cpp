/*
Open Tracker License

Terms and Conditions

Copyright (c) 1991-2000, Be Incorporated. All rights reserved.

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions:

The above copyright notice and this permission notice applies to all licensees
and shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF TITLE, MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
BE INCORPORATED BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN
AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF, OR IN CONNECTION
WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

Except as contained in this notice, the name of Be Incorporated shall not be
used in advertising or otherwise to promote the sale, use or other dealings in
this Software without prior written authorization from Be Incorporated.

Tracker(TM), Be(R), BeOS(R), and BeIA(TM) are trademarks or registered trademarks
of Be Incorporated in the United States and other countries. Other brand product
names are registered trademarks or trademarks of their respective holders.
All rights reserved.
*/


#include "QueryPoseView.h"

#include <new>

#include <Catalog.h>
#include <Debug.h>
#include <Locale.h>
#include <NodeMonitor.h>
#include <Query.h>
#include <Volume.h>
#include <VolumeRoster.h>
#include <Window.h>

#include "Attributes.h"
#include "AttributeStream.h"
#include "AutoLock.h"
#include "Commands.h"
#include "FindPanel.h"
#include "FSUtils.h"
#include "MimeTypeList.h"
#include "MimeTypes.h"
#include "Tracker.h"

#include <fs_attr.h>


using std::nothrow;


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "QueryPoseView"


// Currently filtering out Trash doesn't node monitor too well - if you
// remove an item from the Trash, it doesn't show up in the query result
// To do this properly, we would have to node monitor everything BQuery
// returns and after a node monitor re-chech if it should be part of
// query results and add/remove appropriately. Right now only moving to
// Trash is supported


//	#pragma mark - BQueryPoseView


BQueryPoseView::BQueryPoseView(Model* model)
	:
	BPoseView(model, kListMode),
	fRefFilter(NULL),
	fQueryList(NULL),
	fQueryListContainer(NULL),
	fCreateOldPoseList(false)
{
}


BQueryPoseView::~BQueryPoseView()
{
	delete fQueryListContainer;
}


bool
FolderFilterFunction(const entry_ref* directory, const entry_ref* model)
{
	if (directory == NULL || model == NULL)
		return false;

	BPath directoryPath(directory);
	BPath modelPath(model);

	if (directoryPath.InitCheck() != B_OK || modelPath.InitCheck() != B_OK)
		return false;

	char* requiredDirectoryPath = const_cast<char*>(directoryPath.Path());
	strcat(requiredDirectoryPath, "/");
	// only supports searching completely in the directories as well as subdirectories for now.
	return strncmp(requiredDirectoryPath, modelPath.Path(), strlen(requiredDirectoryPath)) == 0;
}


bool
QueryRefFilter::PassThroughDirectoryFilters(const entry_ref* ref) const
{
	int32 count = fDirectoryFilters.CountItems();
	bool passed = count == 0;
		// in this context, even if the model passes through a single filter, it is considered
		// as the folder selections are combined with OR! and not AND!

	for (int32 i = 0; i < count; i++) {
		entry_ref* filterDirectory = fDirectoryFilters.ItemAt(i);
		if (FolderFilterFunction(filterDirectory, ref)) {
			passed = true;
			break;
		}
	}

	return passed;
}


void
BQueryPoseView::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kFSClipboardChanges:
		{
			// poses have always to be updated for the query view
			UpdatePosesClipboardModeFromClipboard(message);
			break;
		}

		default:
			_inherited::MessageReceived(message);
			break;
	}
}


void
BQueryPoseView::AdoptSystemColors()
{
	SetViewUIColor(B_DOCUMENT_BACKGROUND_COLOR, ReadOnlyTint(B_DOCUMENT_BACKGROUND_COLOR));
	SetLowUIColor(ViewUIColor());
	SetHighUIColor(B_DOCUMENT_TEXT_COLOR);
}


bool
BQueryPoseView::HasSystemColors() const
{
	float tint = B_NO_TINT;
	float readOnlyTint = ReadOnlyTint(B_DOCUMENT_BACKGROUND_COLOR);

	return ViewUIColor(&tint) == B_DOCUMENT_BACKGROUND_COLOR && tint == readOnlyTint
		&& LowUIColor(&tint) == B_DOCUMENT_BACKGROUND_COLOR && tint == readOnlyTint
		&& HighUIColor(&tint) == B_DOCUMENT_TEXT_COLOR && tint == B_NO_TINT;
}


void
BQueryPoseView::EditQueries()
{
	BMessage message(kEditQuery);
	message.AddRef("refs", TargetModel()->EntryRef());
	BMessenger(kTrackerSignature, -1, 0).SendMessage(&message);
}


void
BQueryPoseView::SetupDefaultColumnsIfNeeded()
{
	// in case there were errors getting some columns
	if (CountColumns() != 0)
		return;

	AddColumn(new BColumn(B_TRANSLATE("Name"), 145,
		B_ALIGN_LEFT, kAttrStatName, B_STRING_TYPE, true, true));
	AddColumn(new BColumn(B_TRANSLATE("Location"), 225,
		B_ALIGN_LEFT, kAttrPath, B_STRING_TYPE, true, false));
	AddColumn(new BColumn(B_TRANSLATE("Size"), 80,
		B_ALIGN_RIGHT, kAttrStatSize, B_OFF_T_TYPE, true, false));
	AddColumn(new BColumn(B_TRANSLATE("Modified"), 150,
		B_ALIGN_LEFT, kAttrStatModified, B_TIME_TYPE, true, false));
}


void
BQueryPoseView::RestoreState(AttributeStreamNode* node)
{
	_inherited::RestoreState(node);
	fViewState->SetViewMode(kListMode);
}


void
BQueryPoseView::RestoreState(const BMessage &message)
{
	_inherited::RestoreState(message);
	fViewState->SetViewMode(kListMode);
}


void
BQueryPoseView::SavePoseLocations(BRect*)
{
}


void
BQueryPoseView::SetViewMode(uint32)
{
}


void
BQueryPoseView::OpenParent()
{
}


void
BQueryPoseView::Refresh()
{
	PRINT(("refreshing dynamic date query\n"));

	// cause the old AddPosesTask to die
	fAddPosesThreads.clear();
	delete fQueryListContainer;
	fQueryListContainer = NULL;

	fCreateOldPoseList = true;
	AddPoses(TargetModel());
	TargetModel()->CloseNode();

	ResetOrigin();
	ResetPosePlacementHint();
}


void
BQueryPoseView::AddPosesCompleted()
{
	ASSERT(Window()->IsLocked());

	PoseList* oldPoseList = fQueryListContainer->OldPoseList();
	if (oldPoseList != NULL) {
		int32 count = oldPoseList->CountItems();
		for (int32 index = count - 1; index >= 0; index--) {
			BPose* pose = oldPoseList->ItemAt(index);
			DeletePose(pose->TargetModel()->NodeRef());
		}
		fQueryListContainer->ClearOldPoseList();
	}

	_inherited::AddPosesCompleted();
}


// When using dynamic dates, such as "today", need to refresh the query
// window every now and then

EntryListBase*
BQueryPoseView::InitDirentIterator(const entry_ref* ref)
{
	BEntry entry(ref);
	if (entry.InitCheck() != B_OK)
		return NULL;

	Model sourceModel(&entry, true);
	if (sourceModel.InitCheck() != B_OK)
		return NULL;

	ASSERT(sourceModel.IsQuery());

	// old pose list is used for finding poses that no longer match a
	// dynamic date query during a Refresh call
	PoseList* oldPoseList = NULL;
	if (fCreateOldPoseList) {
		oldPoseList = new PoseList(10);
		oldPoseList->AddList(fPoseList);
	}

	fQueryListContainer = new QueryEntryListCollection(&sourceModel, this,
		oldPoseList);
	fCreateOldPoseList = false;

	if (fQueryListContainer->InitCheck() != B_OK) {
		delete fQueryListContainer;
		fQueryListContainer = NULL;
		return NULL;
	}

	TTracker::WatchNode(sourceModel.NodeRef(), B_WATCH_NAME | B_WATCH_STAT
		| B_WATCH_ATTR, this);

	fQueryList = fQueryListContainer->QueryList();

	if (fQueryListContainer->DynamicDateQuery()) {
		// calculate the time to trigger the query refresh - next midnight

		time_t now = time(0);

		time_t nextMidnight = now + 60 * 60 * 24;
			// move ahead by a day
		tm timeData;
		localtime_r(&nextMidnight, &timeData);
		timeData.tm_sec = 0;
		timeData.tm_min = 0;
		timeData.tm_hour = 0;
		nextMidnight = mktime(&timeData);

		time_t nextHour = now + 60 * 60;
			// move ahead by a hour
		localtime_r(&nextHour, &timeData);
		timeData.tm_sec = 0;
		timeData.tm_min = 0;
		nextHour = mktime(&timeData);

		PRINT(("%" B_PRIdTIME " minutes, %" B_PRIdTIME " seconds till next hour\n",
			(nextHour - now) / 60, (nextHour - now) % 60));

		time_t nextMinute = now + 60;
			// move ahead by a minute
		localtime_r(&nextMinute, &timeData);
		timeData.tm_sec = 0;
		nextMinute = mktime(&timeData);

		PRINT(("%" B_PRIdTIME " seconds till next minute\n", nextMinute - now));

		bigtime_t delta;
		if (fQueryListContainer->DynamicDateRefreshEveryMinute())
			delta = nextMinute - now;
		else if (fQueryListContainer->DynamicDateRefreshEveryHour())
			delta = nextHour - now;
		else
			delta = nextMidnight - now;

#if DEBUG
		int32 secondsTillMidnight = (nextMidnight - now);
		int32 minutesTillMidnight = secondsTillMidnight/60;
		secondsTillMidnight %= 60;
		int32 hoursTillMidnight = minutesTillMidnight/60;
		minutesTillMidnight %= 60;

		PRINT(("%" B_PRId32 " hours, %" B_PRId32 " minutes, %" B_PRId32
			" seconds till midnight\n", hoursTillMidnight, minutesTillMidnight,
			secondsTillMidnight));

		int32 refreshInSeconds = delta % 60;
		int32 refreshInMinutes = delta / 60;
		int32 refreshInHours = refreshInMinutes / 60;
		refreshInMinutes %= 60;

		PRINT(("next refresh in %" B_PRId32 " hours, %" B_PRId32 "minutes, %"
			B_PRId32 " seconds\n", refreshInHours, refreshInMinutes,
			refreshInSeconds));
#endif

		// bump up to microseconds
		delta *= 1000000;

		TTracker* tracker = dynamic_cast<TTracker*>(be_app);
		ThrowOnAssert(tracker != NULL);

		tracker->MainTaskLoop()->RunLater(
			NewLockingMemberFunctionObject(&BQueryPoseView::Refresh, this),
			delta);
	}

	QueryRefFilter* filter = new QueryRefFilter(fQueryListContainer->ShowResultsFromTrash());
	TargetModel()->OpenNode();
	filter->LoadDirectoryFiltersFromFile(TargetModel()->Node());
	TargetModel()->CloseNode();
	SetRefFilter(filter);

	return fQueryListContainer->Clone();
}


uint32
BQueryPoseView::WatchNewNodeMask()
{
	return B_WATCH_NAME | B_WATCH_STAT | B_WATCH_ATTR;
}


const char*
BQueryPoseView::SearchForType() const
{
	if (!fSearchForMimeType.Length()) {
		BModelOpener opener(TargetModel());
		BString buffer;
		attr_info attrInfo;

		// read the type of files we are looking for
		status_t status
			= TargetModel()->Node()->GetAttrInfo(kAttrQueryInitialMime,
				&attrInfo);
		if (status == B_OK) {
			TargetModel()->Node()->ReadAttrString(kAttrQueryInitialMime,
				&buffer);
		}

		TTracker* tracker = dynamic_cast<TTracker*>(be_app);
		if (tracker != NULL && buffer.Length() > 0) {
			const ShortMimeInfo* info = tracker->MimeTypes()->FindMimeType(
				buffer.String());
			if (info != NULL)
				fSearchForMimeType = info->InternalName();
		}

		if (!fSearchForMimeType.Length())
			fSearchForMimeType = B_FILE_MIMETYPE;
	}

	return fSearchForMimeType.String();
}


bool
BQueryPoseView::ActiveOnDevice(dev_t device) const
{
	int32 count = fQueryList->CountItems();
	for (int32 index = 0; index < count; index++) {
		if (fQueryList->ItemAt(index)->TargetDevice() == device)
			return true;
	}

	return false;
}


//	#pragma mark - QueryRefFilter


QueryRefFilter::QueryRefFilter(bool showResultsFromTrash)
	:
	fShowResultsFromTrash(showResultsFromTrash)
{
}


QueryRefFilter::~QueryRefFilter()
{
	int32 count = fDirectoryFilters.CountItems();
	for (int32 i = 0; i < count; i++)
		delete fDirectoryFilters.RemoveItemAt(0);
}


status_t
QueryRefFilter::LoadDirectoryFiltersFromFile(const BNode* node)
{
	// params checking
	if (node == NULL || node->InitCheck() != B_OK)
		return B_BAD_VALUE;

	struct attr_info info;
	status_t error = node->GetAttrInfo("_trk/directories", &info);
	if (error != B_OK)
		return error;

	BString bufferString;
	char* buffer = bufferString.LockBuffer(info.size);
	if (node->ReadAttr("_trk/directories", B_MESSAGE_TYPE, 0, buffer, info.size) != info.size)
		return B_ERROR;

	BMessage message;
	error = message.Unflatten(buffer);
	if (error != B_OK)
		return error;

	int32 count;
	if ((error = message.GetInfo("refs", NULL, &count)) != B_OK)
		return error;

	for (int32 i = 0; i < count; i++) {
		entry_ref ref;
		if ((error = message.FindRef("refs", i, &ref)) != B_OK)
			continue;

		AddDirectoryFilter(&ref);
	}

	return B_OK;
}


status_t
QueryRefFilter::AddDirectoryFilter(const entry_ref* ref)
{
	if (ref == NULL)
		return B_BAD_VALUE;

	// checking for duplicates
	int32 count = fDirectoryFilters.CountItems();
	for (int32 i = 0; i < count; i++) {
		entry_ref* item = fDirectoryFilters.ItemAt(i);
		if (ref != NULL && item != NULL && *item == *ref)
			return B_CANCELED;
	}

	BEntry entry(ref, true);
	if (entry.InitCheck() != B_OK || !entry.Exists() || !entry.IsDirectory())
		return B_ERROR;

	entry_ref symlinkTraversedRef;
	entry.GetRef(&symlinkTraversedRef);

	fDirectoryFilters.AddItem(new entry_ref(symlinkTraversedRef));
	return B_OK;
}


bool
QueryRefFilter::Filter(const entry_ref* ref, BNode* node, stat_beos* st,
	const char* filetype)
{
	TTracker* tracker = dynamic_cast<TTracker*>(be_app);
	return !(!fShowResultsFromTrash && tracker != NULL && tracker->InTrashNode(ref))
		&& PassThroughDirectoryFilters(ref);
}


//	#pragma mark - QueryEntryListCollection


QueryEntryListCollection::QueryEntryListCollection(Model* model,
	BHandler* target, PoseList* oldPoseList)
	:
	fQueryListRep(new QueryListRep(new BObjectList<BQuery, true>(5)))
{
	Rewind();
	attr_info info;
	BQuery query;

	BNode* modelNode = model->Node();
	if (modelNode == NULL) {
		fStatus = B_ERROR;
		return;
	}

	// read the actual query string
	fStatus = modelNode->GetAttrInfo(kAttrQueryString, &info);
	if (fStatus != B_OK)
		return;

	BString buffer;
	if (modelNode->ReadAttr(kAttrQueryString, B_STRING_TYPE, 0,
		buffer.LockBuffer((int32)info.size),
			(size_t)info.size) != info.size) {
		fStatus = B_ERROR;
		return;
	}

	buffer.UnlockBuffer();

	// read the extra options
	MoreOptionsStruct saveMoreOptions;
	if (ReadAttr(modelNode, kAttrQueryMoreOptions,
			kAttrQueryMoreOptionsForeign, B_RAW_TYPE, 0, &saveMoreOptions,
			sizeof(MoreOptionsStruct),
			&MoreOptionsStruct::EndianSwap) != kReadAttrFailed) {
		fQueryListRep->fShowResultsFromTrash = saveMoreOptions.searchTrash;
	}

	fStatus = query.SetPredicate(buffer.String());

	fQueryListRep->fOldPoseList = oldPoseList;
	fQueryListRep->fDynamicDateQuery = false;

	fQueryListRep->fRefreshEveryHour = false;
	fQueryListRep->fRefreshEveryMinute = false;

	if (modelNode->ReadAttr(kAttrDynamicDateQuery, B_BOOL_TYPE, 0,
			&fQueryListRep->fDynamicDateQuery,
			sizeof(bool)) != sizeof(bool)) {
		fQueryListRep->fDynamicDateQuery = false;
	}

	if (fQueryListRep->fDynamicDateQuery) {
		// only refresh every minute on debug builds
		fQueryListRep->fRefreshEveryMinute = buffer.IFindFirst("second") != -1
			|| buffer.IFindFirst("minute") != -1;
		fQueryListRep->fRefreshEveryHour = fQueryListRep->fRefreshEveryMinute
			|| buffer.IFindFirst("hour") != -1;

#if !DEBUG
		// don't refresh every minute unless we are running debug build
		fQueryListRep->fRefreshEveryMinute = false;
#endif
	}

	if (fStatus != B_OK)
		return;

	bool searchAllVolumes = true;
	status_t result = B_OK;

	// get volumes to perform query on
	if (modelNode->GetAttrInfo(kAttrQueryVolume, &info) == B_OK) {
		char* buffer = NULL;

		if ((buffer = (char*)malloc((size_t)info.size)) != NULL
			&& modelNode->ReadAttr(kAttrQueryVolume, B_MESSAGE_TYPE, 0,
				buffer, (size_t)info.size) == info.size) {
			BMessage message;
			if (message.Unflatten(buffer) == B_OK) {
				for (int32 index = 0; ;index++) {
					ASSERT(index < 100);
					BVolume volume;
						// match a volume with the info embedded in
						// the message
					result = MatchArchivedVolume(&volume, &message, index);
					if (result == B_OK) {
						// start the query on this volume
						result = FetchOneQuery(&query, target,
							fQueryListRep->fQueryList, &volume);
						if (result != B_OK)
							continue;

						searchAllVolumes = false;
					} else if (result != B_DEV_BAD_DRIVE_NUM) {
						// if B_DEV_BAD_DRIVE_NUM, the volume just isn't
						// mounted this time around, keep looking for more
						// if other error, bail
						break;
					}
				}
			}
		}

		free(buffer);
	}

	if (searchAllVolumes) {
		// no specific volumes embedded in query, search everything
		BVolumeRoster roster;
		BVolume volume;

		roster.Rewind();
		while (roster.GetNextVolume(&volume) == B_OK)
			if (volume.IsPersistent() && volume.KnowsQuery()) {
				result = FetchOneQuery(&query, target,
					fQueryListRep->fQueryList, &volume);
				if (result != B_OK)
					continue;
			}
	}

	fStatus = B_OK;

	return;
}


status_t
QueryEntryListCollection::FetchOneQuery(const BQuery* copyThis,
	BHandler* target, BObjectList<BQuery, true>* list, BVolume* volume)
{
	BQuery* query = new (nothrow) BQuery;
	if (query == NULL)
		return B_NO_MEMORY;

	// have to fake a copy constructor here because BQuery doesn't have
	// a copy constructor
	BString buffer;
	const_cast<BQuery*>(copyThis)->GetPredicate(&buffer);
	query->SetPredicate(buffer.String());

	query->SetTarget(BMessenger(target));
	query->SetVolume(volume);

	status_t result = query->Fetch();
	if (result != B_OK) {
		PRINT(("fetch error %s\n", strerror(result)));
		delete query;
		return result;
	}

	list->AddItem(query);

	return B_OK;
}


QueryEntryListCollection::~QueryEntryListCollection()
{
	if (fQueryListRep->CloseQueryList())
		delete fQueryListRep;
}


QueryEntryListCollection*
QueryEntryListCollection::Clone()
{
	fQueryListRep->OpenQueryList();
	return new QueryEntryListCollection(*this);
}


//	#pragma mark - QueryEntryListCollection


QueryEntryListCollection::QueryEntryListCollection(
	const QueryEntryListCollection &cloneThis)
	:
	EntryListBase(),
	fQueryListRep(cloneThis.fQueryListRep)
{
	// only to be used by the Clone routine
}


void
QueryEntryListCollection::ClearOldPoseList()
{
	delete fQueryListRep->fOldPoseList;
	fQueryListRep->fOldPoseList = NULL;
}


status_t
QueryEntryListCollection::GetNextEntry(BEntry* entry, bool traverse)
{
	status_t result = B_ERROR;

	for (int32 count = fQueryListRep->fQueryList->CountItems();
		fQueryListRep->fQueryListIndex < count;
		fQueryListRep->fQueryListIndex++) {
		result = fQueryListRep->fQueryList->
			ItemAt(fQueryListRep->fQueryListIndex)->
				GetNextEntry(entry, traverse);
		if (result == B_OK)
			break;
	}

	return result;
}


int32
QueryEntryListCollection::GetNextDirents(struct dirent* buffer, size_t length,
	int32 count)
{
	int32 result = 0;

	for (int32 queryCount = fQueryListRep->fQueryList->CountItems();
			fQueryListRep->fQueryListIndex < queryCount;
			fQueryListRep->fQueryListIndex++) {
		result = fQueryListRep->fQueryList->
			ItemAt(fQueryListRep->fQueryListIndex)->
				GetNextDirents(buffer, length, count);
		if (result > 0)
			break;
	}

	return result;
}


status_t
QueryEntryListCollection::GetNextRef(entry_ref* ref)
{
	status_t result = B_ERROR;

	for (int32 count = fQueryListRep->fQueryList->CountItems();
		fQueryListRep->fQueryListIndex < count;
		fQueryListRep->fQueryListIndex++) {

		result = fQueryListRep->fQueryList->
			ItemAt(fQueryListRep->fQueryListIndex)->GetNextRef(ref);
		if (result == B_OK)
			break;
	}

	return result;
}


status_t
QueryEntryListCollection::Rewind()
{
	fQueryListRep->fQueryListIndex = 0;

	return B_OK;
}


int32
QueryEntryListCollection::CountEntries()
{
	return 0;
}


bool
QueryEntryListCollection::ShowResultsFromTrash() const
{
	return fQueryListRep->fShowResultsFromTrash;
}


bool
QueryEntryListCollection::DynamicDateQuery() const
{
	return fQueryListRep->fDynamicDateQuery;
}


bool
QueryEntryListCollection::DynamicDateRefreshEveryHour() const
{
	return fQueryListRep->fRefreshEveryHour;
}


bool
QueryEntryListCollection::DynamicDateRefreshEveryMinute() const
{
	return fQueryListRep->fRefreshEveryMinute;
}
