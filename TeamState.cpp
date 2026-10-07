#include "TeamState.h"
#include "GpuContexts.h"
#include "RadeonDevice.h"
#include "RadeonServer.h"
#include <stdio.h>
#include <string.h>
#include <algorithm>


volatile bool gServerStopping = false;


ExternalPtr<TeamRoster> gTeamRoster = MakeExternal<TeamRoster>();


//#pragma mark - RadeonServerThreadLink

RadeonServerThreadLink::RadeonServerThreadLink(port_id clientPort):
	ServerThreadLink(clientPort)
{
	fTeamState = gTeamRoster.Switch()->ThisTeam(ClientTeam(), true);
	auto teamStateLocked = fTeamState.Switch();
	teamStateLocked->fThreadLinks.Insert(this);
	fAddedToList = true;
	InitCompleted();
}

RadeonServerThreadLink::~RadeonServerThreadLink()
{
	printf("-RadeonServerThreadLink, team: %" B_PRId32 "\n", ClientTeam());
	ExternalRef<TeamState> teamState = fTeamState;
	if (!teamState.IsSet()) {
		printf("[!] ~RadeonServerThreadLink(): fTeamState == NULL\n");
		return;
	}
	auto teamStateLocked = teamState.Switch();
	if (!fRemovedFromList) {
		teamStateLocked->fThreadLinks.Remove(this);
		fRemovedFromList = true;
	}
}

ExternalRef<TeamState> RadeonServerThreadLink::StateFor(team_id team)
{
	return gTeamRoster.Switch()->ThisTeam(team, false);
}

ExternalRef<TeamState> RadeonServerThreadLink::ThisState()
{
	return StateFor(ClientTeam());
}


//#pragma mark - TeamState

TeamState::TeamState(team_id team):
	fTeam(team),
	fAddressSpace(new AddressSpace(), true),
	fCsSeq(0),
	fLastCsSeq(0)
{
	printf("+TeamState(%" B_PRId32 ")\n", fTeam);
	fVirtMemPool.Register(0, 0x200000);

	char name[B_OS_NAME_LENGTH];
	snprintf(name, sizeof(name), "%s%" B_PRId32, RADEON_GFX_SYNCOBJ_POINTS_AREA,
		fTeam);
	void *address;
	fSyncobjPointsArea.SetTo(create_area(name, &address, B_ANY_ADDRESS,
		RADEON_GFX_SYNCOBJ_POINTS_COUNT * sizeof(uint64), B_FULL_LOCK,
		B_READ_AREA | B_WRITE_AREA | B_CLONEABLE_AREA));
	if (fSyncobjPointsArea.IsSet())
		fSyncobjPoints = (volatile uint64*)address;
	else
		printf("[!] team %" B_PRId32 ": no syncobj points area\n", fTeam);
}

TeamState::~TeamState()
{
	printf("-TeamState(%" B_PRId32 ")\n", fTeam);
	gGpuContexts.DeleteTeam(fTeam);
	// what the client didn't unmap: kept alive until now
	uint32 mappings;
	uint64 mappedSize;
	fAddressSpace->GetMappingStats(mappings, mappedSize);
	// what the client didn't close
	uint32 buffers = 0, syncobjs = 0;
	uint64 bufferSize[3] = {};
	for (auto &it : fHandles.Handles()) {
		if (it.second.type == HandleType::syncobj) {
			syncobjs++;
			// the area goes away with the team state, the syncobj may not
			volatile uint64 *slot = SyncobjPointSlot(it.first);
			if (slot != NULL)
				static_cast<Syncobj*>(it.second.ref.Get())->UnpublishFrom(slot);
			continue;
		}
		BufferObject *buffer = static_cast<BufferObject*>(it.second.ref.Get());
		if (buffer == NULL)
			continue;
		buffers++;
		if (buffer->domain >= boDomainVram && buffer->domain <= boDomainGtt)
			bufferSize[buffer->domain] += buffer->size;
	}
	if (buffers > 0 || syncobjs > 0) {
		printf("  left open: %" B_PRIu32 " buffers (VRAM %" B_PRIu64
			" KB, visible VRAM %" B_PRIu64 " KB, GTT %" B_PRIu64 " KB), %"
			B_PRIu32 " syncobjs\n", buffers, bufferSize[boDomainVram] >> 10,
			bufferSize[boDomainVramMappable] >> 10, bufferSize[boDomainGtt] >> 10,
			syncobjs);
	}
	if (mappings > 0 || fClosedWhileMapped > 0 || fFailedVaOps > 0) {
		printf("  left mapped: %" B_PRIu32 " mappings, %" B_PRIu64 " KB; %"
			B_PRIu32 " handles closed while mapped, %" B_PRIu32
			" VA operations failed\n", mappings, mappedSize >> 10,
			fClosedWhileMapped, fFailedVaOps);
	}
	// left over memory hints at leaks
	if (auto memMgr = gDevice.MemMgr().Switch()) {
		uint64 total, vram, vramMappable, gtt;
		memMgr->GetUsage(total, vram, boDomainVram);
		memMgr->GetUsage(total, vramMappable, boDomainVramMappable);
		memMgr->GetUsage(total, gtt, boDomainGtt);
		printf("  memory in use: VRAM %" B_PRIu64 " MB, visible VRAM %" B_PRIu64
			" MB, GTT %" B_PRIu64 " MB\n", vram >> 20, vramMappable >> 20,
			gtt >> 20);
	}
}

void TeamState::FirstReferenceAcquired()
{
	atomic_or((int32*)&fFlags.val, Flags{.reacquired = true}.val);
}

void TeamState::LastReferenceReleased()
{
	Flags oldFlags {.val = atomic_or((int32*)&fFlags.val, Flags{.finalized = true}.val)};
	auto teamRoster = gTeamRoster.Switch();
	if (!oldFlags.finalized) {
		auto it = teamRoster->fTeamStates.find(fTeam);
		if (it == teamRoster->fTeamStates.end()) abort();
		teamRoster->fTeamStates.erase(it);
	}

	oldFlags.val = atomic_and((int32*)&fFlags.val, ~Flags{.reacquired = true}.val);
	if (!oldFlags.reacquired) {
		if (!WaitForSubmissions())
			return;
		RefObject::LastReferenceReleased();
	}
}

/*!	A killed client's submissions can still be on the GPU, and their fence
	handlers retire them in this team's domain: the team's buffers, page
	tables and domain must stay until they are done. Not in the team's
	domain. Waits until a hang reset would have completed them; returns
	false if they still aren't: the state is kept then (a leak instead of
	the GPU writing to freed memory).
*/
bool TeamState::WaitForSubmissions()
{
	if (CurrentDomain() == GetDomain()) {
		printf("[!] team %" B_PRId32 ": released in its own domain, can't wait "
			"for its submissions\n", fTeam);
		return true;
	}
	// a hung GPU is reset after the lockup timeout, which completes them
	bigtime_t start = system_time();
	bigtime_t deadline = start + gLockupTimeout + 5000000;
	for (bool first = true;; first = false) {
		BReference<Fence> fence;
		size_t count;
		{
			auto teamState = ExternalPtr<TeamState>(this).Switch();
			if (fCmdSubs.empty()) {
				if (!first) {
					printf("team %" B_PRId32 ": submissions done after %"
						B_PRId64 " us\n", fTeam, system_time() - start);
				}
				return true;
			}
			count = fCmdSubs.size();
			if (first) {
				printf("team %" B_PRId32 ": waiting for %" B_PRIuSIZE
					" submission(s)\n", fTeam, count);
			}
			fence = fCmdSubs.rbegin()->second->fence;
		}
		// in slices: a stopping server halts the GPU and doesn't wait
		status_t status;
		do {
			status = fence->WaitNonDomain(B_ABSOLUTE_TIMEOUT,
				std::min(deadline, system_time() + 100000));
		} while (status == B_TIMED_OUT && system_time() < deadline
			&& !gServerStopping);
		if (status < B_OK && gServerStopping) {
			printf("team %" B_PRId32 ": server stopping, %" B_PRIuSIZE
				" submission(s) not done\n", fTeam, count);
			return false;
		}
		if (status < B_OK) {
			printf("[!] team %" B_PRId32 ": %" B_PRIuSIZE " submission(s) not "
				"done, keeping its memory\n", fTeam, count);
			return false;
		}
		// the fence handler retires the submission in the team's domain
		if (system_time() > deadline) {
			printf("[!] team %" B_PRId32 ": %" B_PRIuSIZE " submission(s) not "
				"retired, keeping its memory\n", fTeam, count);
			return false;
		}
	}
}

void TeamState::Terminate()
{
	for (;;) {
		RadeonServerThreadLink *threadLink = fThreadLinks.First();
		if (threadLink == NULL) break;
		threadLink->fRemovedFromList = true;
		fThreadLinks.Remove(threadLink);
		threadLink->Close();
	}
}

volatile uint64 *TeamState::SyncobjPointSlot(int32 handle)
{
	if (fSyncobjPoints == NULL || handle < 0
		|| (uint32)handle >= RADEON_GFX_SYNCOBJ_POINTS_COUNT) {
		return NULL;
	}
	return &fSyncobjPoints[handle];
}

int32 TeamState::RegisterHandle(const Handle &handleObj)
{
	int32 handle = fHandles.Register(handleObj);
	if (handle >= 0 && handleObj.type == HandleType::syncobj) {
		volatile uint64 *slot = SyncobjPointSlot(handle);
		if (slot != NULL)
			static_cast<Syncobj*>(handleObj.ref.Get())->PublishTo(slot);
	}
	return handle;
}

status_t TeamState::FreeHandle(int32 handle)
{
	Handle handleObj = fHandles.This(handle);
	if (handleObj.type == HandleType::syncobj && handleObj.ref.IsSet()) {
		volatile uint64 *slot = SyncobjPointSlot(handle);
		if (slot != NULL)
			static_cast<Syncobj*>(handleObj.ref.Get())->UnpublishFrom(slot);
	}
	if (handleObj.type == HandleType::buffer && handleObj.ref.IsSet()
		&& fAddressSpace->CountMappings(
			static_cast<BufferObject*>(handleObj.ref.Get())) > 0) {
		fClosedWhileMapped++;
	}
	return fHandles.Free(handle);
}


void TeamState::VaOpFailed(uint32 operation, uint64 address, status_t status)
{
	if (fFailedVaOps++ < 5) {
		printf("[!] team %" B_PRId32 ": VA operation %" B_PRIu32 " at %#"
			B_PRIx64 " failed: %s\n", fTeam, operation, address,
			strerror(status));
	}
}

TeamState::Handle TeamState::ThisHandle(int32 handle)
{
	return fHandles.This(handle);
}

BReferenceable *TeamState::ThisHandle(int32 handle, HandleType type)
{
	const Handle &handleObj = fHandles.This(handle);
	if (handleObj.type != HandleType::buffer) return NULL;
	BReferenceable *ref = handleObj.ref.Get();
	ref->AcquireReference();
	return ref;
}


//#pragma mark - Buffers

int32 TeamState::AllocBuffer(MemoryDomain domain, uint64 size, uint64 alignment, uint32 flags, area_id area, uint64 offset)
{
	void *clonedAreaAdr = NULL;
	// TODO: read only area support
	AreaDeleter clonedArea;
	if (area >= B_OK) {
		clonedArea.SetTo(clone_area("userptr", &clonedAreaAdr, B_ANY_ADDRESS, B_READ_AREA | B_WRITE_AREA, area));
		CheckRet(clonedArea.Get());
		area_info info;
		CheckRet(get_area_info(clonedArea.Get(), &info));
		if (size < offset || info.size - offset < size) return B_ERROR;
		switch (info.lock) {
			case B_FULL_LOCK:
			case B_CONTIGUOUS:
			case B_LOMEM:
			case B_32_BIT_FULL_LOCK:
			case B_32_BIT_CONTIGUOUS:
				break;
			default:
				return B_ERROR; // area physical memory must be locked
		}
	}
	BReference<BufferObject> buffer = gDevice.MemMgr().Switch()->Alloc(domain, size, alignment, flags, clonedArea.Get(), offset);
	if (!buffer.IsSet()) {
		printf("buffer alloc failed, domain: %d, size: %#" B_PRIx64 "\n", (int)domain, size);
		return B_NO_MEMORY;
	}
	clonedArea.Detach();
	return RegisterHandle(Handle{.type = HandleType::buffer, .ref = buffer});
}

BReference<BufferObject> TeamState::ThisBuffer(int32 handle)
{
	const Handle &handleObj = fHandles.This(handle);
	if (handleObj.type != HandleType::buffer) return NULL;
	return BReference<BufferObject>((BufferObject*)handleObj.ref.Get(), false);
}

status_t TeamState::Map(uint64 virtAdr, int32 handle, uint64 offset, uint64 size, uint32 flags)
{
	//printf("\nMap(%#" B_PRIx64 ", %" B_PRId32 ", %#" B_PRIx64 ", %#" B_PRIx64 ")\n", virtAdr, handle, offset, size);
	(void)flags;
	BReference<BufferObject> buffer = ThisBuffer(handle);
	if (!buffer.IsSet()) return B_ERROR;
	status_t res = fAddressSpace->Map(buffer, virtAdr, offset, size);
	if (res < B_OK) return res;
	return res;
}

status_t TeamState::Unmap(uint64 virtAdr, int32 handle, uint64 offset, uint64 size, uint32 flags)
{
	(void)flags;
	//printf("\nUnmap(%#" B_PRIx64 ", %" B_PRId32 ", %#" B_PRIx64 ", %#" B_PRIx64 ")\n", virtAdr, handle, offset, size);
	BReference<BufferObject> buffer = ThisBuffer(handle);
	if (!buffer.IsSet()) return B_ERROR;
	return fAddressSpace->Unmap(buffer, virtAdr, offset, size);
}

status_t TeamState::CpuMap(void *&adr, int32 handle, uint64 offset, uint64 size, uint32 flags)
{
	(void)flags;
	BReference<BufferObject> buffer = ThisBuffer(handle);
	if (!buffer.IsSet()) return B_ERROR;
	return gDevice.MemMgr().Switch()->CpuMap(adr, buffer, offset, size);
}

status_t TeamState::CpuUnmap(void *adr, int32 handle, uint64 offset, uint64 size, uint32 flags)
{
	(void)adr;
	(void)handle;
	(void)offset;
	(void)size;
	(void)flags;
	return B_OK;
}


//#pragma mark - Syncobjs

int32 TeamState::CreateSyncobj(Syncobj::CreateFlags flags)
{
	SyncobjRef syncobj(new Syncobj(flags), true);
	return RegisterHandle(Handle{.type = HandleType::syncobj, .ref = syncobj});
}

SyncobjRef TeamState::ThisSyncobj(int32 handle)
{
	const Handle &handleObj = fHandles.This(handle);
	if (handleObj.type != HandleType::syncobj) return NULL;
	return SyncobjRef((Syncobj*)handleObj.ref.Get(), false);
}

void TeamState::DumpSyncobjs()
{
	for (auto it = fHandles.Handles().begin(); it != fHandles.Handles().end(); it++) {
		int32 handle = it->first;
		const Handle &handleObj = it->second;
		if (handleObj.type == HandleType::syncobj) {
			SyncobjRef syncobj((Syncobj*)handleObj.ref.Get(), false);
			printf("%" B_PRId32 "(%p): ", handle, syncobj.Get());
			syncobj->Dump();
		}
	}
	printf("\n");
}


//#pragma mark - Command submissions

status_t TeamState::ScheduleCS(uint64 &handle, CommandSubmission *cs)
{
	if (GetDomain() != CurrentDomain()) {
		printf("[!] Domain() != CurrentDomain()\n");
		abort();
	}
	// a bad submission is rejected before it gets a sequence number
	bigtime_t start = CsStatsEnabled() ? system_time() : 0;
	status_t status = cs->Remap();
	if (start != 0) {
		bigtime_t now = system_time();
		CsStatsAdd(kCsStageRemap, now - start);
		start = now;
	}
	if (status < B_OK) {
		printf("[!] CS rejected: IBs can't be remapped\n");
		delete cs;
		return status;
	}
	handle = fLastCsSeq++;
	cs->seq = handle;
	//printf("TeamState::ScheduleCS() -> %" B_PRIu64 "\n", handle);

	CheckRet(cs->Schedule());
	fCmdSubs.emplace(handle, cs);
	if (start != 0)
		CsStatsAdd(kCsStageSchedule, system_time() - start);

	return B_OK;
}

// the fence of a submission, unset if it is done
status_t TeamState::CsFence(uint64 handle, BReference<Fence> &fence)
{
	fence.Unset();
	if ((int32)fCsSeq - (int32)handle >= 0)
		return B_OK;
	auto it = fCmdSubs.find((uint32)handle);
	if (it == fCmdSubs.end())
		return B_ERROR;
	fence = it->second->fence;
	return B_OK;
}


//#pragma mark - TeamRoster

ExternalRef<TeamState> TeamRoster::ThisTeam(team_id team, bool create)
{
	auto it = fTeamStates.find(team);
	if (it == fTeamStates.end()) {
		if (!create) return NULL;
		ExternalRef<TeamState> newTeamState(MakeExternal<TeamState>(team), true);
		fTeamStates.emplace(team, newTeamState);
		return newTeamState;
	}
	return ExternalRef<TeamState>(it->second, false);
}

int32 TeamRoster::CountTeams()
{
	return fTeamStates.size();
}

void TeamRoster::ListTeams(bool (*Handler)(ExternalRef<TeamState> ts, void *arg), void *arg)
{
	for (auto it = fTeamStates.begin(); it != fTeamStates.end(); it++) {
		if (!Handler(it->second, arg)) return;
	}
}
