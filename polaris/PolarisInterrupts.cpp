#include "PolarisInterrupts.h"
#include "RadeonDevice.h"
#include "RingBuffer.h"

#include <stdio.h>

#include <OS.h>


// how often the interrupt thread looks at the IH ring
static const bigtime_t kPollInterval = 250;
// how often fences are checked without an interrupt
static const bigtime_t kFenceCheckInterval = 10000;


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
	return count > 0;
}


status_t
PolarisRingBufferInt::WaitForInterrupt()
{
	snooze(kPollInterval);
	return B_OK;
}
