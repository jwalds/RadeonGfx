// "RadeonGfx gfxtest": the graphics command processor. The SMU loads the
// CE/PFP/ME/MEC/RLC firmware, ring 0 is started with the clear state
// preamble, then: a scratch register write, memory writes to VRAM and
// through the GART, and an end-of-pipe fence with an interrupt.
// Everything is halted and restored at the end.

#include "PolarisGfxTest.h"
#include "PolarisGfx.h"
#include "PolarisIh.h"
#include "PolarisSdma.h"
#include "PolarisSmu.h"
#include "RenderDevice.h"
#include "RadeonDevice.h"
#include "Radeon.h"

#include <stdio.h>
#include <string.h>
#include <image.h>
#include <OS.h>
#include <Path.h>

#include "vi/gfx_8_0_d.h"

#define CheckRet(err) {status_t _err = (err); if (_err < B_OK) return _err;}

static const bigtime_t kTimeout = 1000000;
static const uint32 kSrcIdCpEndOfPipe = 181;


static status_t
FirmwareDir(BPath &path)
{
	image_info info;
	int32 cookie = 0;
	while (get_next_image_info(B_CURRENT_TEAM, &cookie, &info) == B_OK) {
		if (info.type != B_APP_IMAGE)
			continue;
		CheckRet(path.SetTo(info.name));
		CheckRet(path.GetParent(&path));
		CheckRet(path.GetParent(&path));
		return path.Append("firmware");
	}
	return B_ENTRY_NOT_FOUND;
}


static status_t
Run(PolarisGfx &gfx, const char *what)
{
	gfx.Commit();
	status_t status = gfx.WaitIdle(kTimeout);
	if (status < B_OK) {
		printf("  [!] %s: CP didn't finish within %" B_PRIdBIGTIME " ms\n",
			what, kTimeout / 1000);
		gfx.PrintState();
	}
	return status;
}


struct EopState {
	uint32 eops;
	uint32 others;
};


static void
HandleVector(const PolarisIhRing::Entry &entry, void *cookie)
{
	EopState &state = *(EopState*)cookie;
	if (entry.srcId == kSrcIdCpEndOfPipe) {
		state.eops++;
		printf("  IH vector: source %" B_PRIu32 " (CP end of pipe), data %#"
			B_PRIx32 ", ring %#" B_PRIx32 "\n", entry.srcId, entry.srcData,
			entry.ringId);
	} else
		state.others++;
}


static status_t
RunTests(PolarisGfx &gfx, PolarisIhRing &ih)
{
	// the clear state preamble from Init()
	CheckRet(Run(gfx, "clear state"));
	printf("0. clear state preamble: OK\n");

	// 1. gfx_v8_0_ring_test_ring()
	WriteReg4AmdGpu(mmSCRATCH_REG0, 0xcafedead);
	CheckRet(gfx.Begin(8));
	gfx.EmitSetUconfigReg(mmSCRATCH_REG0, 0xdeadbeef);
	CheckRet(Run(gfx, "scratch register"));
	uint32 scratch = ReadReg4AmdGpu(mmSCRATCH_REG0);
	printf("1. SET_UCONFIG_REG SCRATCH_REG0: %#010" B_PRIx32 " %s\n", scratch,
		scratch == 0xdeadbeef ? "OK" : "[!] FAILED");
	if (scratch != 0xdeadbeef)
		return B_ERROR;

	auto memMgr = gDevice.MemMgr().Switch();
	MappedBuffer vram(memMgr->Alloc(boDomainVramMappable, B_PAGE_SIZE));
	MappedBuffer system(memMgr->Alloc(boDomainGtt, B_PAGE_SIZE));
	if (vram.adr == NULL || system.adr == NULL)
		return B_NO_MEMORY;
	volatile uint32 *vramWords = (volatile uint32*)vram.adr;
	volatile uint32 *systemWords = (volatile uint32*)system.adr;
	vramWords[0] = 0;
	systemWords[0] = 0;
	PolarisFlushHdp();

	// 2. WRITE_DATA to VRAM and to system memory through the GART
	CheckRet(gfx.Begin(16));
	gfx.EmitWriteData(vram.buf->gpuPhysAdr, 0x12345678);
	gfx.EmitWriteData(system.buf->gpuPhysAdr, 0x9abcdef0);
	CheckRet(Run(gfx, "write data"));
	PolarisInvalidateHdp();
	bool ok = vramWords[0] == 0x12345678 && systemWords[0] == 0x9abcdef0;
	printf("2. WRITE_DATA: VRAM %#010" B_PRIx32 ", system memory %#010" B_PRIx32
		" %s\n", vramWords[0], systemWords[0], ok ? "OK" : "[!] FAILED");
	if (!ok)
		return B_ERROR;

	// 3. end-of-pipe fence with an interrupt
	EopState state = {};
	ih.Poll(HandleVector, &state);
	state = {};
	gfx.EnableEopInterrupt(true);
	CheckRet(gfx.Begin(16));
	gfx.EmitFence(system.buf->gpuPhysAdr + 16, 3, true);
	CheckRet(Run(gfx, "fence"));
	bigtime_t start = system_time();
	while ((systemWords[4] != 3 || state.eops == 0)
		&& system_time() - start < 100000) {
		ih.Poll(HandleVector, &state);
		snooze(100);
	}
	gfx.EnableEopInterrupt(false);
	ok = systemWords[4] == 3 && state.eops > 0;
	printf("3. EVENT_WRITE_EOP fence %" B_PRIu32 ", %" B_PRIu32
		" interrupt(s): %s\n", systemWords[4], state.eops,
		ok ? "OK" : "[!] FAILED");
	return ok ? B_OK : B_ERROR;
}


status_t
PolarisGfxTest()
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

	BPath firmwareDir;
	CheckRet(FirmwareDir(firmwareDir));
	BPath smcFirmware(firmwareDir.Path(), "polaris11_smc.bin");

	status = gDevice.MemMgr().Switch()->InitGartPolaris();
	PolarisIhRing ih;
	PolarisSmu smu;
	PolarisGfx gfx;
	if (status >= B_OK)
		status = ih.Init();

	if (status >= B_OK && !smu.IsFirmwareRunning())
		status = smu.Start(smcFirmware.Path());
	else if (status >= B_OK)
		printf("SMU:       firmware already running\n");
	if (status >= B_OK)
		status = smu.LoadAllFirmware(firmwareDir.Path());
	printf("SMU:       %" B_PRIu32 " C\n", smu.Temperature());

	if (status >= B_OK)
		status = gfx.Init();
	if (status >= B_OK)
		status = RunTests(gfx, ih);
	else
		printf("[!] setup failed: %s\n", strerror(status));

	gfx.Fini();
	ih.Fini();
	gDevice.MemMgr().Switch()->FiniGartPolaris();
	printf("SMU:       %" B_PRIu32 " C\n", smu.Temperature());
	printf("%s\n", status >= B_OK ? "all done" : "[!] test failed");
	return status;
}
