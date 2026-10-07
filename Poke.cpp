#include "Poke.h"
#include <errno.h>


Poke gPoke;


Poke::Poke(): fFd(open("/dev/misc/poke", B_READ_WRITE))
{
	if (!fFd.IsSet()) abort();
}

status_t Poke::GetPhysicalAddress(uint64 &physAdr, void* address, size_t size)
{
	mem_map_args args{
		.signature = POKE_SIGNATURE,
		.size = size,
		.address = address
	};
	status_t res = ioctl(fFd.Get(), POKE_GET_PHYSICAL_ADDRESS, &args, sizeof(args));
	if (res < 0) return errno;
	physAdr = (uint64)args.physical_address;
	return res;
}

status_t Poke::GetPhysicalRun(uint64 &physAdr, size_t &runSize, void* address,
	size_t size)
{
	mem_map_args args{
		.signature = POKE_SIGNATURE,
		.size = size,
		.address = address
	};
	// one table entry: B_BUFFER_OVERFLOW when the range has more runs, the
	// first one is filled in anyway
	status_t res = ioctl(fFd.Get(), POKE_GET_PHYSICAL_ADDRESS, &args, sizeof(args));
	if (res < 0 && errno != B_BUFFER_OVERFLOW) return errno;
	if (args.size == 0) return B_ERROR;
	physAdr = (uint64)args.physical_address;
	runSize = args.size;
	return B_OK;
}

status_t Poke::GetNthPciInfo(uint32 index, pci_info &info)
{
	if (index > UINT8_MAX) return ENOENT;
	pci_info_args args{
		.signature = POKE_SIGNATURE,
		.index = (uint8)index,
		.info = &info
	};
	status_t res = ioctl(fFd.Get(), POKE_GET_NTH_PCI_INFO, &args, sizeof(args));
	if (res < 0) return errno;
	return args.status;
}
