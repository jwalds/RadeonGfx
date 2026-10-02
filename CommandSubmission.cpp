#include "CommandSubmission.h"
#include "TeamState.h"
#include "Units/GfxV6RingPackets.h"
#include "FenceGroup.h"
#include "bif_3_0_d.h"
#include <stdio.h>
#include <stdlib.h>


static void Accumulate(BReference<Fence> &dst, BReference<Fence> src)
{
	if (!dst.IsSet()) {
		dst = src;
		return;
	}
	BReference<Fence> fences[] = {dst, src};
	dst.SetTo(new FenceGroup(fences, B_COUNT_OF(fences), {.all = true}), true);
}


CommandSubmission::CommandSubmission(TeamState *teamState):
	teamState(teamState),
	fenceResolvedReq(teamState)
{
	//printf("+%p.CommandSubmission()\n", this);
}

CommandSubmission::~CommandSubmission()
{
	//printf("-%p.CommandSubmission()\n", this);
	for (uint32 i = 0; i < indBufCnt; i++) {
		IndirectBufferDef &ib = indBufs[i];
		// a submission rejected while parsing was never remapped
		if (!ib.remapped)
			continue;
		uint64 sizeAligned = RoundUp<uint64>(ib.size, B_PAGE_SIZE);
		teamState->fVirtMemPool.Free(ib.vaRemapped);
		if (teamState->fAddressSpace->Unmap(NULL, ib.vaRemapped, 0, sizeAligned) < B_OK) abort();
	}
}

status_t CommandSubmission::Schedule()
{
	fence.SetTo(new RingFence(), true);
	fence->OnSignal(&handler);
	if (Remap() < B_OK) {
		printf("[!] CS: Remap() failed\n");
		abort();
	}
	//CheckRet(cs->Remap());

	waitFenceGroup = SignaledFence::Instance();
	for (uint32 i = 0; i < waitCnt; i++) {
		Accumulate(waitFenceGroup, waitSyncobjs[i]->LookupFence(waitPoints[i], Syncobj::WaitFlags{.domain = true, .forSubmit = true}));
	}
	waitFenceGroup->OnSignal(&waitHandler);
	for (uint32 i = 0; i < signalCnt; i++) {
		signalSyncobjs[i]->InsertFence(signalPoints[i], fence);
	}

	return B_OK;
}

status_t CommandSubmission::Remap()
{
	for (uint32 i = 0; i < indBufCnt; i++) {
		IndirectBufferDef &ib = indBufs[i];
		uint64 sizeAligned = RoundUp<uint64>(ib.size, B_PAGE_SIZE);
		uint64 offset;
		ib.buf = teamState->fAddressSpace->Lookup(ib.va, offset);
		if (!ib.buf.IsSet()) {
			printf("[!] CS: IB at %#" B_PRIx64 " (%" B_PRIu64 " bytes) not mapped\n",
				ib.va, (uint64)ib.size);
			return B_ERROR;
		}
		if (!teamState->fVirtMemPool.Alloc(ib.vaRemapped, sizeAligned)) {
			printf("[!] CS: no address space to remap an IB of %" B_PRIu64 " bytes\n",
				sizeAligned);
			return B_ERROR;
		}
		status_t status = teamState->fAddressSpace->Map(ib.buf, ib.vaRemapped,
			offset, sizeAligned);
		if (status < B_OK) {
			printf("[!] CS: remapping the IB at %#" B_PRIx64 " to %#" B_PRIx64
				" failed\n", ib.va, ib.vaRemapped);
			teamState->fVirtMemPool.Free(ib.vaRemapped);
			return status;
		}
		ib.remapped = true;
	}
	return B_OK;
}


void CommandSubmission::WaitHandler::Do(Fence *fence)
{
	auto cs = &Base();
	auto ts = ExternalPtr<TeamState>(Base().teamState).Switch();
	//printf("CommandSubmission::WaitHandler::Do\n");

	ExternalPtr<RadeonRingBuffer> ringExt = gDevice.Rings(RADEON_RING_TYPE_GFX_INDEX);
	int32 vmId = ts->fAddressSpace->AcquireVmid(); // can lock
	if (vmId < B_OK) {
		printf("[!] can't allocate VMID\n");
		abort();
	}
	{
		auto ring = ringExt.Switch();
		uint32 cmdSize = /*5 +*/ 24 + 12*cs->indBufCnt + 14;
		if (cs->userFence.buffer.IsSet()) {
			cmdSize += 14;
		}

		WriteReg4AmdGpu(mmHDP_MEM_COHERENCY_FLUSH_CNTL, 0x1);
		/* bits 0-15 are the VM contexts0-15 */
		//WriteReg4AmdGpu(mmVM_INVALIDATE_REQUEST, 1 << vmId);

		//bigtime_t t1 = system_time();
		ring->Begin(cmdSize); // can lock
		//GenFlushHdp(*ring);
		static const bool trace = getenv("RADEONGFX_TRACE") != NULL;
		if (trace) {
			printf("CS seq %" B_PRIu64 ": VMID %" B_PRId32 ", page directory %#"
				B_PRIx64 ", %" B_PRIu32 " IB(s):", (uint64)cs->seq, vmId,
				ts->fAddressSpace->PageDir(), cs->indBufCnt);
			for (uint32 i = 0; i < cs->indBufCnt; i++) {
				printf(" %#" B_PRIx64 " (%" B_PRIu64 " dwords)",
					cs->indBufs[i].vaRemapped, (uint64)cs->indBufs[i].size / 4);
			}
			printf("\n");
		}
		ring->WriteVmFlush(vmId, ts->fAddressSpace->PageDir());
		for (uint32 i = 0; i < cs->indBufCnt; i++) {
			ring->WriteIb(cs->indBufs[i].vaRemapped, cs->indBufs[i].size/4, vmId);
		}
		if (cs->userFence.buffer.IsSet()) {
			ring->WriteUserFence(cs->userFence.buffer->gpuPhysAdr + cs->userFence.offset, cs->seq);
		}
		ring->WriteFence(cs->fence);
		ring->End();
	}
}

void CommandSubmission::FenceHandler::Do(Fence *fence)
{
	Base().fenceResolvedReq.Schedule();
}

void CommandSubmission::FenceResolvedReq::Do(Object *obj)
{
	(void)obj;
	auto ts = Base().teamState;
	ts->fCsSeq = Base().seq;
	//printf("TeamState::Resolved(%" B_PRIu32 ")\n", Base().seq);
	auto it = ts->fCmdSubs.find((uint32)Base().seq);
	if (it == ts->fCmdSubs.end()) {
		printf("[!] CS: resolved submission %" B_PRIu64 " unknown\n", (uint64)Base().seq);
		abort();
	}
	ts->fCmdSubs.erase(it);
	ts->fAddressSpace->ReleaseVmid();
}

void CommandSubmission::FenceResolvedReq::Resolved()
{
	delete &Base();
}
