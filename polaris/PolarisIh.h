#pragma once

#include "RadeonMemory.h"
#include <private/shared/AutoDeleterOS.h>


// Interrupt handler ring (IH 3.0, Linux tonga_ih.c). The GPU writes 16 byte
// interrupt vectors into a ring in system memory. The IH doesn't go through
// the GPU's VM: like Linux (use_bus_addr), the ring and write pointer use
// physical bus addresses of contiguous memory. The CPU interrupt stays
// disabled (radeon_hd has no interrupt handler), so the ring is polled.
class PolarisIhRing {
public:
	struct Entry {
		uint32 srcId;
		uint32 srcData;
		uint32 ringId;
		uint32 vmId;
		uint32 pasId;
		uint32 raw[4];
	};

	typedef void (*Handler)(const Entry &entry, void *cookie);

	PolarisIhRing();
	~PolarisIhRing();

	status_t Init(uint32 ringSize = 64 * 1024);
	void Fini();

	// handles all pending vectors, returns how many
	uint32 Poll(Handler handler, void *cookie);
	bool Overflowed() {return fOverflowed;}
	void PrintState();

private:
	AreaDeleter fArea;			// ring followed by one page for the wptr
	volatile uint32 *fRing;
	volatile uint32 *fWptr;
	uint64 fRingPhys;
	uint64 fWptrPhys;
	uint32 fRingSize;
	uint32 fRptr;
	bool fOverflowed;
	bool fEnabled;
};
