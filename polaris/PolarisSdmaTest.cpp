// "RadeonGfx sdmatest": first work for the GPU. Loads the SDMA0 firmware,
// starts its ring and runs write, fill and copy packets in VRAM and through
// the GART, then a fence with a trap into the IH ring. Everything is halted
// and restored at the end, also after a failure.

#include "PolarisSdmaTest.h"
#include "PolarisSdma.h"
#include "PolarisIh.h"
#include "RenderDevice.h"
#include "RadeonDevice.h"
#include "Radeon.h"

#include <stdio.h>
#include <string.h>
#include <image.h>
#include <OS.h>
#include <Path.h>

#define CheckRet(err) {status_t _err = (err); if (_err < B_OK) return _err;}

static const bigtime_t kTimeout = 1000000;
static const uint32 kTestSize = 1024 * 1024;
static const uint32 kSrcIdSdmaTrap = 224;


static status_t
FirmwarePath(BPath &path, const char *name)
{
	// <RadeonGfx checkout>/firmware, next to the build directory
	image_info info;
	int32 cookie = 0;
	while (get_next_image_info(B_CURRENT_TEAM, &cookie, &info) == B_OK) {
		if (info.type != B_APP_IMAGE)
			continue;
		CheckRet(path.SetTo(info.name));
		CheckRet(path.GetParent(&path));
		CheckRet(path.GetParent(&path));
		return path.Append(BString("firmware/") << name);
	}
	return B_ENTRY_NOT_FOUND;
}


static status_t
Run(PolarisSdma &sdma, const char *what)
{
	sdma.Commit();
	status_t status = sdma.WaitIdle(kTimeout);
	if (status < B_OK) {
		printf("  [!] %s: engine didn't finish within %" B_PRIdBIGTIME
			" ms\n", what, kTimeout / 1000);
		sdma.PrintState();
	}
	return status;
}


static bool
CheckWords(const volatile uint32 *words, uint32 count, uint32 expected,
	const char *what)
{
	for (uint32 i = 0; i < count; i++) {
		if (words[i] != expected) {
			printf("  [!] %s: word %" B_PRIu32 " is %#010" B_PRIx32
				", expected %#010" B_PRIx32 "\n", what, i, words[i], expected);
			return false;
		}
	}
	return true;
}


static uint32
Pattern(uint32 seed, uint32 i)
{
	return seed ^ (i * 2654435761u);
}


static bool
CheckPattern(const volatile uint32 *words, uint32 count, uint32 seed,
	const char *what)
{
	for (uint32 i = 0; i < count; i++) {
		if (words[i] != Pattern(seed, i)) {
			printf("  [!] %s: word %" B_PRIu32 " is %#010" B_PRIx32
				", expected %#010" B_PRIx32 "\n", what, i, words[i],
				Pattern(seed, i));
			return false;
		}
	}
	return true;
}


struct TrapState {
	uint32 traps;
	uint32 others;
};


static void
HandleVector(const PolarisIhRing::Entry &entry, void *cookie)
{
	TrapState &state = *(TrapState*)cookie;
	if (entry.srcId == kSrcIdSdmaTrap) {
		state.traps++;
		printf("  IH vector: source %" B_PRIu32 " (SDMA trap), data %#"
			B_PRIx32 ", ring %" B_PRIu32 "\n", entry.srcId, entry.srcData,
			entry.ringId);
	} else
		state.others++;
}


static status_t
RunTests(PolarisSdma &sdma, PolarisIhRing &ih)
{
	auto memMgr = gDevice.MemMgr().Switch();
	MappedBuffer scratch(memMgr->Alloc(boDomainVramMappable, B_PAGE_SIZE));
	MappedBuffer source(memMgr->Alloc(boDomainVramMappable, kTestSize));
	MappedBuffer destination(memMgr->Alloc(boDomainVramMappable, kTestSize));
	MappedBuffer system(memMgr->Alloc(boDomainGtt, kTestSize));
	if (scratch.adr == NULL || source.adr == NULL || destination.adr == NULL
		|| system.adr == NULL)
		return B_NO_MEMORY;
	volatile uint32 *scratchWords = (volatile uint32*)scratch.adr;
	uint64 scratchAddress = scratch.buf->gpuPhysAdr;
	uint32 words = kTestSize / 4;

	// 1. a single write (Linux sdma_v3_0_ring_test_ring)
	scratchWords[0] = 0xcafedead;
	PolarisFlushHdp();
	CheckRet(sdma.Begin(8));
	sdma.EmitWrite(scratchAddress, 0xdeadbeef);
	CheckRet(Run(sdma, "write"));
	PolarisInvalidateHdp();
	printf("1. write to VRAM %#" B_PRIx64 ": %#010" B_PRIx32 " %s\n",
		scratchAddress, scratchWords[0],
		scratchWords[0] == 0xdeadbeef ? "OK" : "[!] FAILED");
	if (scratchWords[0] != 0xdeadbeef)
		return B_ERROR;

	// 2. fill
	CheckRet(sdma.Begin(16));
	sdma.EmitFill(destination.buf->gpuPhysAdr, 0xa5a5a5a5, kTestSize);
	sdma.EmitFence(scratchAddress, 2);
	CheckRet(Run(sdma, "fill"));
	PolarisInvalidateHdp();
	bool ok = scratchWords[0] == 2 && CheckWords(
		(volatile uint32*)destination.adr, words, 0xa5a5a5a5, "fill");
	printf("2. fill 1 MB of VRAM with 0xa5a5a5a5, fence: %s\n",
		ok ? "OK" : "[!] FAILED");
	if (!ok)
		return B_ERROR;

	// 3. VRAM to VRAM copy
	for (uint32 i = 0; i < words; i++)
		((volatile uint32*)source.adr)[i] = Pattern(0x11110000, i);
	PolarisFlushHdp();
	CheckRet(sdma.Begin(16));
	sdma.EmitCopy(source.buf->gpuPhysAdr, destination.buf->gpuPhysAdr,
		kTestSize);
	sdma.EmitFence(scratchAddress, 3);
	CheckRet(Run(sdma, "copy"));
	PolarisInvalidateHdp();
	ok = scratchWords[0] == 3 && CheckPattern(
		(volatile uint32*)destination.adr, words, 0x11110000, "copy");
	printf("3. copy 1 MB VRAM -> VRAM: %s\n", ok ? "OK" : "[!] FAILED");
	if (!ok)
		return B_ERROR;

	// 4. VRAM to system memory through the GART
	memset(system.adr, 0, kTestSize);
	CheckRet(sdma.Begin(16));
	sdma.EmitCopy(source.buf->gpuPhysAdr, system.buf->gpuPhysAdr, kTestSize);
	sdma.EmitFence(scratchAddress, 4);
	CheckRet(Run(sdma, "copy to GTT"));
	PolarisInvalidateHdp();
	ok = scratchWords[0] == 4 && CheckPattern((volatile uint32*)system.adr,
		words, 0x11110000, "copy to GTT");
	printf("4. copy 1 MB VRAM -> system memory (GART %#" B_PRIx64 "): %s\n",
		system.buf->gpuPhysAdr, ok ? "OK" : "[!] FAILED");
	if (!ok)
		return B_ERROR;

	// 5. system memory to VRAM through the GART
	for (uint32 i = 0; i < words; i++)
		((uint32*)system.adr)[i] = Pattern(0x22220000, i);
	CheckRet(sdma.Begin(16));
	sdma.EmitCopy(system.buf->gpuPhysAdr, destination.buf->gpuPhysAdr,
		kTestSize);
	sdma.EmitFence(scratchAddress, 5);
	CheckRet(Run(sdma, "copy from GTT"));
	PolarisInvalidateHdp();
	ok = scratchWords[0] == 5 && CheckPattern(
		(volatile uint32*)destination.adr, words, 0x22220000, "copy from GTT");
	printf("5. copy 1 MB system memory -> VRAM: %s\n", ok ? "OK" : "[!] FAILED");
	if (!ok)
		return B_ERROR;

	// 6. fence with a trap into the IH ring
	TrapState trapState = {};
	ih.Poll(HandleVector, &trapState);
	trapState = {};
	sdma.EnableTrap(true);
	CheckRet(sdma.Begin(16));
	sdma.EmitFence(scratchAddress, 6);
	sdma.EmitTrap(0);
	CheckRet(Run(sdma, "fence and trap"));
	bigtime_t start = system_time();
	while (trapState.traps == 0 && system_time() - start < 100000) {
		ih.Poll(HandleVector, &trapState);
		snooze(100);
	}
	sdma.EnableTrap(false);
	PolarisInvalidateHdp();
	printf("6. fence %" B_PRIu32 " and trap: %" B_PRIu32 " trap vector(s) %s\n",
		scratchWords[0], trapState.traps,
		trapState.traps > 0 && scratchWords[0] == 6 ? "OK" : "[!] FAILED");

	// 7. speed: fill 64 MB of (invisible) VRAM
	const uint32 kChunk = 0x3fffe0 & ~(uint32)0xfffff;	// 3 MB
	BReference<BufferObject> big = memMgr->Alloc(boDomainVram,
		64 * 1024 * 1024);
	if (big.IsSet()) {
		uint32 chunks = 64 * 1024 * 1024 / kChunk;
		CheckRet(sdma.Begin(chunks * 5 + 8));
		for (uint32 i = 0; i < chunks; i++) {
			sdma.EmitFill(big->gpuPhysAdr + (uint64)i * kChunk, 0x00000000,
				kChunk);
		}
		sdma.EmitFence(scratchAddress, 7);
		start = system_time();
		CheckRet(Run(sdma, "64 MB fill"));
		bigtime_t elapsed = system_time() - start;
		double bytes = (double)chunks * kChunk;
		printf("7. fill %.0f MB of VRAM: %.2f ms, %.1f GB/s\n",
			bytes / (1024 * 1024), elapsed / 1000.0,
			bytes / (elapsed / 1e6) / 1e9);
	}
	return B_OK;
}


status_t
PolarisSdmaTest()
{
	FileDescriptorCloser fd;
	BString path;
	if (OpenRenderDevice(fd, path) < B_OK) {
		printf("no radeon_hd render device found in /dev/graphics\n");
		return B_ENTRY_NOT_FOUND;
	}
	status_t status = gDevice.InitPolaris(fd.Get(), true);
	if (status < B_OK) {
		printf("[!] attaching to %s failed: %s\n", path.String(),
			strerror(status));
		return status;
	}
	printf("Device:    %s (registers writable)\n", path.String());
	CheckRet(gDevice.MemMgr().Switch()->InitPolaris());

	BPath firmware;
	CheckRet(FirmwarePath(firmware, "polaris11_sdma.bin"));

	status = gDevice.MemMgr().Switch()->InitGartPolaris();
	PolarisIhRing ih;
	PolarisSdma sdma;
	if (status >= B_OK)
		status = ih.Init();
	if (status >= B_OK)
		status = sdma.Init(firmware.Path());
	if (status >= B_OK)
		status = RunTests(sdma, ih);
	else
		printf("[!] setup failed: %s\n", strerror(status));

	sdma.Fini();
	ih.Fini();
	gDevice.MemMgr().Switch()->FiniGartPolaris();
	printf("%s\n", status >= B_OK ? "all done" : "[!] test failed");
	return status;
}
