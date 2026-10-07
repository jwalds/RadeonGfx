#pragma once

#include <AutoDeleter.h>
#include <AutoDeleterOS.h>
#include <private/drivers/poke.h>


class Poke {
private:
	FileDescriptorCloser fFd;

public:
	Poke();

	status_t GetPhysicalAddress(uint64 &physAdr, void* address, size_t size);
	// the physically contiguous run at address (at most size bytes)
	status_t GetPhysicalRun(uint64 &physAdr, size_t &runSize, void* address,
		size_t size);
	status_t GetNthPciInfo(uint32 index, pci_info &info);
};


extern Poke gPoke;
