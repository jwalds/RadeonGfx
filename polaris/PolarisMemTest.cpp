// "RadeonGfx memtest": Polaris memory step. Registers are mapped read-only;
// only VRAM outside the display driver's blocks is written, through the CPU.

#include "PolarisMemTest.h"
#include "RenderDevice.h"
#include "RadeonDevice.h"
#include "RadeonMemory.h"
#include "Poke.h"

#include <stdio.h>
#include <string.h>
#include <OS.h>

#define CheckRet(err) {status_t _err = (err); if (_err < B_OK) return _err;}


static void
PrintUsage(MemoryManager &memMgr)
{
	static const char *kNames[] = {"VRAM", "VRAM visible", "GTT"};
	for (int32 domain = boDomainVram; domain <= boDomainGtt; domain++) {
		uint64 total, alloc;
		memMgr.GetUsage(total, alloc, (MemoryDomain)domain);
		printf("  %-13s %6" B_PRIu64 " / %6" B_PRIu64 " KB used\n",
			kNames[domain], alloc / 1024, total / 1024);
	}
}


static bool
FillAndCheck(MappedBuffer &buffer, uint32 seed)
{
	uint32 *words = (uint32*)buffer.adr;
	size_t count = buffer.buf->size / sizeof(uint32);
	for (size_t i = 0; i < count; i++)
		words[i] = seed ^ (uint32)(i * 2654435761u);
	for (size_t i = 0; i < count; i++) {
		if (words[i] != (seed ^ (uint32)(i * 2654435761u))) {
			printf("  [!] mismatch at word %" B_PRIuSIZE "\n", i);
			return false;
		}
	}
	return true;
}


static status_t
TestVram(MemoryManager &memMgr)
{
	static const uint64 kSizes[] = {B_PAGE_SIZE, 1024 * 1024, 16 * 1024 * 1024};
	MappedBuffer buffers[B_COUNT_OF(kSizes)];

	printf("VRAM buffers (CPU visible)\n");
	for (uint32 i = 0; i < B_COUNT_OF(kSizes); i++) {
		buffers[i].SetTo(memMgr.Alloc(boDomainVramMappable, kSizes[i],
			64 * 1024));
		if (buffers[i].adr == NULL) {
			printf("  [!] allocation of %" B_PRIu64 " KB failed\n",
				kSizes[i] / 1024);
			return B_NO_MEMORY;
		}
		bool ok = FillAndCheck(buffers[i], 0x5a5a0000 + i);
		printf("  %6" B_PRIu64 " KB at %#" B_PRIx64 ": %s\n", kSizes[i] / 1024,
			buffers[i].buf->gpuPhysAdr, ok ? "write/read OK" : "FAILED");
		if (!ok)
			return B_ERROR;
		if (buffers[i].buf->gpuPhysAdr % (64 * 1024) != 0) {
			printf("  [!] not 64 KB aligned\n");
			return B_ERROR;
		}
	}

	BReference<BufferObject> big = memMgr.Alloc(boDomainVram,
		1024 * 1024 * 1024);
	if (!big.IsSet()) {
		printf("  [!] 1 GB VRAM allocation failed\n");
		return B_NO_MEMORY;
	}
	printf("VRAM buffer (not CPU visible)\n  1 GB at %#" B_PRIx64 "\n",
		big->gpuPhysAdr);

	printf("usage with the test buffers\n");
	PrintUsage(memMgr);
	return B_OK;
}


static status_t
TestSystemMemory()
{
	// GTT buffers: locked system memory the GPU reaches through the GART
	// (set up in a later step); check what physical addresses we get
	const size_t size = 4 * 1024 * 1024;
	void *address = NULL;
	AreaDeleter area(create_area("polaris memtest", &address, B_ANY_ADDRESS,
		size, B_FULL_LOCK, B_READ_AREA | B_WRITE_AREA));
	if (!area.IsSet())
		return area.Get();
	memset(address, 0xa5, size);

	// physical addresses through the poke driver, as GartMap() does
	uint64 first = 0, highest = 0, previous = 0;
	uint32 runs = 0;
	for (size_t offset = 0; offset < size; offset += B_PAGE_SIZE) {
		uint64 physical;
		CheckRet(gPoke.GetPhysicalAddress(physical, (uint8*)address + offset,
			B_PAGE_SIZE));
		if (offset == 0)
			first = physical;
		if (offset == 0 || physical != previous + B_PAGE_SIZE)
			runs++;
		if (physical + B_PAGE_SIZE > highest)
			highest = physical + B_PAGE_SIZE;
		previous = physical;
	}
	printf("System memory (locked, for GTT)\n  4 MB in %" B_PRIu32
		" physical runs, first at %#" B_PRIx64 ", highest end %#" B_PRIx64
		"\n", runs, first, highest);
	return B_OK;
}


status_t
PolarisMemTest()
{
	FileDescriptorCloser fd;
	BString path;
	if (OpenRenderDevice(fd, path) < B_OK) {
		printf("no radeon_hd render device found in /dev/graphics\n");
		return B_ENTRY_NOT_FOUND;
	}

	CheckRet(gDevice.InitPolaris(fd.Get(), false));
	printf("Device:    %s (registers read-only)\n", path.String());

	auto memMgr = gDevice.MemMgr().Switch();
	status_t status = memMgr->InitPolaris();
	if (status < B_OK) {
		printf("[!] memory init failed: %s\n", strerror(status));
		return status;
	}
	printf("usage after init\n");
	PrintUsage(*memMgr);

	CheckRet(TestVram(*memMgr));
	printf("usage after freeing the test buffers\n");
	PrintUsage(*memMgr);

	CheckRet(TestSystemMemory());
	return B_OK;
}
