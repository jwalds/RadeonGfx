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


// gfx803, assembled with llvm-mc -arch=amdgcn -mcpu=gfx803:
//	s_lshl_b32 s5, s4, 6			// workgroup id * 64
//	v_add_u32 v0, vcc, s5, v0		// global index
//	v_lshlrev_b32 v1, 2, v0			// byte offset
//	s_mov_b32 s6, 0xc0de0000
//	v_or_b32 v2, s6, v0
//	buffer_store_dword v2, v1, s[0:3], 0 offen
//	s_waitcnt vmcnt(0)
//	s_endpgm
// s[0:3]: buffer descriptor (user data 0-3), s4: workgroup id x
static const uint32 kFillShader[] = {
	0x8e058604, 0x32000005, 0x24020082, 0xbe8600ff, 0xc0de0000, 0x28040006,
	0xe0701000, 0x80000201, 0xbf8c0f70, 0xbf810000
};


static status_t
RunComputeTest(PolarisGfx &gfx, uint64 fenceAddress,
	volatile uint32 *fenceWord)
{
	const uint32 kGroups = 16, kGroupSize = 64;
	const uint32 kCount = kGroups * kGroupSize;

	auto memMgr = gDevice.MemMgr().Switch();
	MappedBuffer shader(memMgr->Alloc(boDomainGtt, B_PAGE_SIZE, B_PAGE_SIZE));
	MappedBuffer output(memMgr->Alloc(boDomainGtt, kCount * 4));
	if (shader.adr == NULL || output.adr == NULL)
		return B_NO_MEMORY;
	memset(shader.adr, 0, B_PAGE_SIZE);
	memcpy(shader.adr, kFillShader, sizeof(kFillShader));
	memset(output.adr, 0, kCount * 4);
	*fenceWord = 0;

	uint64 shaderAddress = shader.buf->gpuPhysAdr;
	uint64 outputAddress = output.buf->gpuPhysAdr;
	// buffer descriptor (V#): base, stride 0, num_records in bytes,
	// dst_sel xyzw, num_format float, data_format 32
	const uint32 descriptor[4] = {
		(uint32)outputAddress,
		(uint32)(outputAddress >> 32) & 0xffff,
		kCount * 4,
		0x00027fac
	};
	const uint32 start[3] = {0, 0, 0};
	const uint32 threads[3] = {kGroupSize, 1, 1};
	const uint32 program[2] = {
		(uint32)(shaderAddress >> 8), (uint32)(shaderAddress >> 40)
	};
	// RSRC1: 4 VGPRs (0), 16 SGPRs (1), FLOAT_MODE 0xc0
	// RSRC2: 4 user SGPRs, workgroup id x
	const uint32 resources[2] = {
		(0 << 0) | (1 << 6) | (0xc0 << 12),
		(4 << 1) | (1 << 7)
	};
	const uint32 cuMask[2] = {0xffffffff, 0xffffffff};

	CheckRet(gfx.Begin(64));
	gfx.EmitSetComputeReg(mmCOMPUTE_START_X, start, 3);
	gfx.EmitSetComputeReg(mmCOMPUTE_NUM_THREAD_X, threads, 3);
	gfx.EmitSetComputeReg(mmCOMPUTE_PGM_LO, program, 2);
	gfx.EmitSetComputeReg(mmCOMPUTE_PGM_RSRC1, resources, 2);
	gfx.EmitSetComputeReg(mmCOMPUTE_RESOURCE_LIMITS, 0);
	gfx.EmitSetComputeReg(mmCOMPUTE_STATIC_THREAD_MGMT_SE0, cuMask, 2);
	gfx.EmitSetComputeReg(mmCOMPUTE_STATIC_THREAD_MGMT_SE2, cuMask, 2);
	gfx.EmitSetComputeReg(mmCOMPUTE_TMPRING_SIZE, 0);
	gfx.EmitSetComputeReg(mmCOMPUTE_USER_DATA_0, descriptor, 4);
	gfx.EmitDispatch(kGroups, 1, 1);
	gfx.EmitCsPartialFlush();
	gfx.EmitFence(fenceAddress, 4, false);
	CheckRet(Run(gfx, "compute dispatch"));

	bigtime_t start2 = system_time();
	while (*fenceWord != 4 && system_time() - start2 < 100000)
		snooze(100);

	const volatile uint32 *words = (const volatile uint32*)output.adr;
	uint32 bad = 0;
	for (uint32 i = 0; i < kCount; i++) {
		if (words[i] != (0xc0de0000 | i)) {
			if (bad < 4) {
				printf("  [!] out[%" B_PRIu32 "] = %#010" B_PRIx32 "\n", i,
					words[i]);
			}
			bad++;
		}
	}
	bool ok = *fenceWord == 4 && bad == 0;
	printf("4. compute shader, %" B_PRIu32 " x %" B_PRIu32 " threads: out[0] "
		"%#010" B_PRIx32 ", out[%" B_PRIu32 "] %#010" B_PRIx32 ", %" B_PRIu32
		" wrong, fence %" B_PRIu32 ": %s\n", kGroups, kGroupSize, words[0],
		kCount - 1, words[kCount - 1], bad, *fenceWord,
		ok ? "OK" : "[!] FAILED");
	return ok ? B_OK : B_ERROR;
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
	if (!ok)
		return B_ERROR;

	return RunComputeTest(gfx, system.buf->gpuPhysAdr + 32, systemWords + 8);
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
