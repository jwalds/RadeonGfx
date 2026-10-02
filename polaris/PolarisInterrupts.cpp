#include "PolarisInterrupts.h"

#include <OS.h>


// how often the interrupt thread looks at the IH ring
static const bigtime_t kPollInterval = 250;


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
	return count > 0;
}


status_t
PolarisRingBufferInt::WaitForInterrupt()
{
	snooze(kPollInterval);
	return B_OK;
}
