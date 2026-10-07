#include "PolarisInterrupts.h"
#include "GpuContexts.h"
#include "RadeonDevice.h"
#include "RingBuffer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <OS.h>


// how often the interrupt thread looks at the IH ring
static const bigtime_t kPollInterval = 250;
// how often fences are checked without an interrupt
static const bigtime_t kFenceCheckInterval = 10000;
// how often the GFX ring is checked for a hang
static const bigtime_t kHangCheckInterval = 100000;


PolarisRingBufferInt::PolarisRingBufferInt()
{
}


PolarisRingBufferInt::~PolarisRingBufferInt()
{
	FiniPolaris();
}


status_t
PolarisRingBufferInt::InitPolaris(uint32 size)
{
	// as Linux' amdgpu.lockup_timeout (ms), 10 s for GFX
	const char *timeout = getenv("RADEONGFX_LOCKUP_TIMEOUT");
	if (timeout != NULL && atoll(timeout) > 0)
		gLockupTimeout = atoll(timeout) * 1000;
	printf("GFX:       lockup timeout %" B_PRId64 " ms\n",
		gLockupTimeout / 1000);

	status_t status = fIh.Init(size);
	if (status < B_OK)
		return status;
	return StartThread();
}


void
PolarisRingBufferInt::FiniPolaris()
{
	StopThread();
	fIh.Fini();
}


bool
PolarisRingBufferInt::Handle()
{
	uint32 count = fIh.Poll([](const PolarisIhRing::Entry &entry,
			void *cookie) {
		InterruptPacket packet{
			.clientId = 0,
			.srcId = entry.srcId,
			.srcData = entry.srcData,
			.ringId = entry.ringId,
			.vmId = entry.vmId
		};
		((PolarisRingBufferInt*)cookie)->Dispatch(packet);
	}, this);

	// An end of pipe interrupt can get lost (seen with Zink under glmark2:
	// the GFX ring ran empty, its last fence was written, nothing signaled
	// it and the client waited forever); check the fences now and then.
	bigtime_t now = system_time();
	if (count == 0 && now - fLastFenceCheck >= kFenceCheckInterval) {
		fLastFenceCheck = now;
		ExternalPtr<RadeonRingBuffer> ringExt
			= gDevice.Rings(RADEON_RING_TYPE_GFX_INDEX);
		if (ringExt.Get() != NULL) {
			auto ring = ringExt.Switch();
			if (ring->HasPassedFences()) {
				if (fMissedInterrupts++ < 10) {
					printf("[!] IH: fence %" B_PRIu32 " passed without an end of"
						" pipe interrupt\n", ring->Rseq());
				}
				ring->UpdateFences();
			}
		}
	} else if (count > 0)
		fLastFenceCheck = now;

	if (now - fLastHangCheck >= kHangCheckInterval) {
		fLastHangCheck = now;
		CheckGfxHang(now);
	}
	return count > 0;
}


/*!	Linux' drm_sched job timeout and amdgpu_job_timedout(): no fence of the
	GFX ring passed for the lockup timeout. Soft recovery (killing the waves
	of the hung job's VMID) first; if the job still doesn't finish, the
	context is marked guilty, the GFX block reset and every pending fence
	completed. Contexts created before the reset see it in
	AMDGPU_CTX_OP_QUERY_STATE2, and RADV reports the device lost to them.
*/
void
PolarisRingBufferInt::CheckGfxHang(bigtime_t now)
{
	ExternalPtr<RadeonRingBuffer> ringExt
		= gDevice.Rings(RADEON_RING_TYPE_GFX_INDEX);
	if (ringExt.Get() == NULL)
		return;

	RadeonRingBuffer::HangInfo hang;
	{
		auto ring = ringExt.Switch();
		if (!ring->CheckHang(now, gLockupTimeout, hang))
			return;
		printf("[!] GFX: ring timeout: fence %" B_PRIu32 " not reached for %"
			B_PRId64 " ms (team %" B_PRId32 ", context %" B_PRIu32 ", VMID %"
			B_PRId32 ")\n", hang.seq, hang.stalled / 1000, hang.team,
			hang.context, hang.vmId);
		ring->PrintHangState();
		if (ring->SoftRecover(hang.vmId, hang.seq)) {
			printf("[!] GFX: ring timeout, but soft recovered (waves of VMID %"
				B_PRId32 " killed)\n", hang.vmId);
			ring->UpdateFences();
			return;
		}
	}

	// the reset counts before the fences complete: the woken waiters must
	// see it
	gGpuContexts.Reset(hang.team, hang.context);
	bigtime_t start = system_time();
	auto ring = ringExt.Switch();
	status_t status = ring->ResetHardware();
	ring->CompleteFences();
	if (status < B_OK) {
		printf("[!] GFX: reset failed: %s; the GPU needs a server restart\n",
			strerror(status));
	} else {
		printf("[!] GFX: reset %" B_PRId32 " done in %" B_PRId64 " us, team %"
			B_PRId32 " context %" B_PRIu32 " guilty\n",
			gGpuContexts.ResetCount(), system_time() - start, hang.team,
			hang.context);
	}
}


status_t
PolarisRingBufferInt::WaitForInterrupt()
{
	static const bigtime_t sInterval = getenv("RADEONGFX_POLL_US") != NULL
		? atoll(getenv("RADEONGFX_POLL_US")) : kPollInterval;
	snooze(sInterval);
	return B_OK;
}
