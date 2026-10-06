#include "GpuContexts.h"

#include <private/shared/AutoLocker.h>


GpuContexts gGpuContexts;
bigtime_t gLockupTimeout = 10000000;


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

typedef AutoLocker<RecursiveLock, AutoLockerLocksLocking<RecursiveLock>>
	ContextsLocker;


uint32
GpuContexts::Create(team_id team)
{
	ContextsLocker lock(&fLock);
	uint32 id = fNextId++;
	if (fNextId == 0)
		fNextId = 1;
	fContexts[std::make_pair(team, id)] = Context{fResetCounter, false};
	return id;
}


status_t
GpuContexts::Delete(team_id team, uint32 id)
{
	ContextsLocker lock(&fLock);
	return fContexts.erase(std::make_pair(team, id)) == 1 ? B_OK : B_BAD_VALUE;
}


void
GpuContexts::DeleteTeam(team_id team)
{
	ContextsLocker lock(&fLock);
	auto it = fContexts.lower_bound(std::make_pair(team, (uint32)0));
	while (it != fContexts.end() && it->first.first == team)
		it = fContexts.erase(it);
}


bool
GpuContexts::Exists(team_id team, uint32 id)
{
	ContextsLocker lock(&fLock);
	return fContexts.find(std::make_pair(team, id)) != fContexts.end();
}


bool
GpuContexts::IsGuilty(team_id team, uint32 id)
{
	ContextsLocker lock(&fLock);
	auto it = fContexts.find(std::make_pair(team, id));
	return it != fContexts.end() && it->second.guilty;
}


status_t
GpuContexts::Query(team_id team, uint32 id, bool &reset, bool &guilty)
{
	ContextsLocker lock(&fLock);
	auto it = fContexts.find(std::make_pair(team, id));
	if (it == fContexts.end())
		return B_BAD_VALUE;
	reset = it->second.resetCounter != fResetCounter;
	guilty = it->second.guilty;
	return B_OK;
}


void
GpuContexts::Reset(team_id guiltyTeam, uint32 guiltyId)
{
	ContextsLocker lock(&fLock);
	fResetCounter++;
	auto it = fContexts.find(std::make_pair(guiltyTeam, guiltyId));
	if (it != fContexts.end())
		it->second.guilty = true;
	if (fSharedCounter != NULL)
		atomic_set(fSharedCounter, fResetCounter);
}


status_t
GpuContexts::InitSharedCounter(const char *areaName)
{
	ContextsLocker lock(&fLock);
	if (fSharedArea >= 0)
		return B_OK;
	void *address;
	fSharedArea = create_area(areaName, &address,
		B_ANY_ADDRESS, B_PAGE_SIZE, B_NO_LOCK,
		B_READ_AREA | B_WRITE_AREA | B_CLONEABLE_AREA);
	if (fSharedArea < B_OK)
		return fSharedArea;
	fSharedCounter = (int32*)address;
	atomic_set(fSharedCounter, fResetCounter);
	return B_OK;
}


int32
GpuContexts::ResetCount()
{
	ContextsLocker lock(&fLock);
	return fResetCounter;
}


uint32
GpuContexts::Count()
{
	ContextsLocker lock(&fLock);
	return fContexts.size();
}
