#pragma once

#include <OS.h>
#include <map>
#include <utility>

#include "Locks/RecursiveLock.h"


// The amdgpu contexts of the clients (DRM_AMDGPU_CTX) and what GPU resets
// did to them, as Linux' amdgpu_ctx: a reset marks every context created
// before it (AMDGPU_CTX_QUERY2_FLAGS_RESET), and the one whose job hung as
// guilty (AMDGPU_CTX_QUERY2_FLAGS_GUILTY); a guilty context can't submit
// anymore (-ECANCELED). RADV reports VK_ERROR_DEVICE_LOST for both.
class GpuContexts {
public:
	uint32 Create(team_id team);
	status_t Delete(team_id team, uint32 id);
	void DeleteTeam(team_id team);

	bool Exists(team_id team, uint32 id);
	bool IsGuilty(team_id team, uint32 id);
	// B_BAD_VALUE for an unknown context
	status_t Query(team_id team, uint32 id, bool &reset, bool &guilty);

	// a GPU reset caused by a job of the given context (team < 0: unknown)
	void Reset(team_id guiltyTeam, uint32 guiltyId);
	// the reset counter for the clients (RADEON_GFX_RESET_COUNTER_AREA)
	status_t InitSharedCounter(const char *areaName);
	int32 ResetCount();
	uint32 Count();

private:
	struct Context {
		int32 resetCounter;
		bool guilty;
	};

	RecursiveLock fLock;
	std::map<std::pair<team_id, uint32>, Context> fContexts;
	uint32 fNextId = 1;
	int32 fResetCounter = 0;
	area_id fSharedArea = -1;
	int32 *fSharedCounter = NULL;
};


extern GpuContexts gGpuContexts;

// no progress of a ring for this long is a hang (Linux' amdgpu.lockup_timeout)
extern bigtime_t gLockupTimeout;
