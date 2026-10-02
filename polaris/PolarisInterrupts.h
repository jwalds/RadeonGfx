#pragma once

#include "RadeonInterrupts.h"
#include "PolarisIh.h"


// The server's interrupt dispatcher on Polaris: the IH 3.0 ring, polled by
// the interrupt thread (radeon_hd doesn't take the GPU's CPU interrupt).
class PolarisRingBufferInt final: public RadeonRingBufferInt {
public:
	PolarisRingBufferInt();
	~PolarisRingBufferInt() override;

	status_t InitPolaris(uint32 size = 64 * 1024);
	void FiniPolaris();

	bool Handle() override;

protected:
	status_t WaitForInterrupt() override;

private:
	PolarisIhRing fIh;
};
