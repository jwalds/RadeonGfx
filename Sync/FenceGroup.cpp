#include "FenceGroup.h"
#include <private/shared/AutoLocker.h>

#include <stdio.h>


template<typename Lockable>
class AutoLockerLocksLocking {
public:
	inline bool Lock(Lockable* lockable)
	{
		return lockable->Acquire() >= B_OK;
	}

	inline void Unlock(Lockable* lockable)
	{
		lockable->Release();
	}
};


void FenceGroup::GroupHandler::Do(Fence *fence)
{
	AutoLocker<RecursiveLock, AutoLockerLocksLocking<RecursiveLock>> lock(&fGroup->fLock);
	if (fGroup->fRemainingCount > 0) {
		fGroup->fRemainingCount--;
		if (fGroup->fRemainingCount == 0) {
			for (uint32 i = 0; i < fGroup->fCount; i++) {
				fGroup->fHandlers[i].fFence->OnSignalCancel(&fGroup->fHandlers[i]);
			}
			lock.Unlock();
			fGroup->Signal();
		}
	}
}

// The group can lose its last reference in another thread while a fence
// runs this handler: it is kept alive meanwhile, and a group whose last
// reference is already gone (its destructor waits for the fence's lock) is
// skipped.
bool FenceGroup::GroupHandler::Retain(BReferenceable *&holder)
{
	if (!fGroup->TryAcquireReference())
		return false;
	holder = fGroup;
	return true;
}

bool FenceGroup::TryAcquireReference()
{
	int32 count = atomic_get(&fReferenceCount);
	while (count > 0) {
		int32 oldCount = atomic_test_and_set(&fReferenceCount, count + 1, count);
		if (oldCount == count)
			return true;
		count = oldCount;
	}
	return false;
}

FenceGroup::FenceGroup(BReference<Fence> *fences, uint32 count, CreateFlags flags)
{
	fFlags.all = flags.all;
	fCount = count;

	for (uint32 i = 0; i < count; i++) {
		FenceGroup *group = dynamic_cast<FenceGroup*>(fences[i].Get());
		if (group != NULL && group->fFlags.all == fFlags.all) {
			fCount += group->fCount - 1;
		}
	}

	fRemainingCount = fFlags.all ? fCount : 1;

	fHandlers.SetTo(new GroupHandler[fCount]);

	uint32 j = 0;
	for (uint32 i = 0; i < count; i++) {
		FenceGroup *group = dynamic_cast<FenceGroup*>(fences[i].Get());
		if (group != NULL && group->fFlags.all == fFlags.all) {
			for (uint32 k = 0; k < group->fCount; k++)
				fHandlers[j++].fFence = group->fHandlers[k].fFence;
		} else
			fHandlers[j++].fFence = fences[i];
	}

	// Only register the handlers once they are all set up: OnSignal() of an
	// already signaled fence runs the handler at once, which can signal the
	// group and cancel all handlers.
	for (j = 0; j < fCount; j++) {
		fHandlers[j].fGroup = this;
		fHandlers[j].fIdx = j;
	}
	for (j = 0; j < fCount && !IsSignaled(); j++)
		fHandlers[j].fFence->OnSignal(&fHandlers[j]);
}

FenceGroup::~FenceGroup()
{
	for (uint32 i = 0; i < fCount; i++) {
		fHandlers[i].fFence->OnSignalCancel(&fHandlers[i]);
	}
}
