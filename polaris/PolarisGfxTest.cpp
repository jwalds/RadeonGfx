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


// s_endpgm
static const uint32 kEmptyShader[] = {0xbf810000};


static status_t
Dispatch(PolarisGfx &gfx, const char *name, uint64 shaderAddress,
	uint64 outputAddress, uint32 outputBytes, uint32 groups,
	uint64 fenceAddress, volatile uint32 *fenceWord, uint32 fenceValue)
{
	const uint32 kGroupSize = 64;
	// buffer descriptor (V#): base, stride 0, num_records in bytes,
	// dst_sel xyzw, num_format float, data_format 32
	const uint32 descriptor[4] = {
		(uint32)outputAddress,
		(uint32)(outputAddress >> 32) & 0xffff,
		outputBytes,
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

	*fenceWord = 0;
	CheckRet(gfx.Begin(64));
	gfx.EmitSetComputeReg(mmCOMPUTE_START_X, start, 3);
	gfx.EmitSetComputeReg(mmCOMPUTE_NUM_THREAD_X, threads, 3);
	gfx.EmitSetComputeReg(mmCOMPUTE_PGM_LO, program, 2);
	gfx.EmitSetComputeReg(mmCOMPUTE_PGM_RSRC1, resources, 2);
	gfx.EmitSetComputeReg(mmCOMPUTE_RESOURCE_LIMITS, 0);
	gfx.EmitSetComputeReg(mmCOMPUTE_STATIC_THREAD_MGMT_SE0, cuMask, 2);
	gfx.EmitSetComputeReg(mmCOMPUTE_STATIC_THREAD_MGMT_SE2, cuMask, 2);
	gfx.EmitSetComputeReg(mmCOMPUTE_TMPRING_SIZE, 0);
	// the waves ran in VMID 9 without this (VM context 1 fault status)
	gfx.EmitSetComputeReg(mmCOMPUTE_VMID, 0);
	gfx.EmitSetComputeReg(mmCOMPUTE_USER_DATA_0, descriptor, 4);
	gfx.EmitDispatch(groups, 1, 1);
	gfx.EmitCsPartialFlush();
	gfx.EmitFence(fenceAddress, fenceValue, false);
	gfx.Commit();

	bigtime_t start2 = system_time();
	while (*fenceWord != fenceValue && system_time() - start2 < 1000000)
		snooze(100);
	if (*fenceWord != fenceValue) {
		printf("  [!] %s: no fence after the dispatch\n", name);
		gfx.PrintState();
		gfx.DumpWaves();
		return B_TIMED_OUT;
	}
	return B_OK;
}


// writes back and drops the CPU cache lines of a buffer
static void
FlushCpuCache(volatile void *address, size_t size)
{
	__builtin_ia32_mfence();
	for (size_t offset = 0; offset < size; offset += 64)
		__builtin_ia32_clflush((const void*)((addr_t)address + offset));
	__builtin_ia32_mfence();
}


static bool
CheckOutput(const volatile uint32 *words, uint32 count, const char *name)
{
	uint32 bad = 0;
	for (uint32 i = 0; i < count; i++) {
		if (words[i] != (0xc0de0000 | i)) {
			if (bad < 4) {
				printf("  [!] %s: out[%" B_PRIu32 "] = %#010" B_PRIx32 "\n",
					name, i, words[i]);
			}
			bad++;
		}
	}
	return bad == 0;
}


static void
DiagnoseGttStore(PolarisGfx &gfx, MappedBuffer &systemOutput,
	MappedBuffer &vramOutput, uint64 fenceAddress, volatile uint32 *fenceWord)
{
	volatile uint32 *system = (volatile uint32*)systemOutput.adr;
	volatile uint32 *vram = (volatile uint32*)vramOutput.adr;
	snooze(100000);
	printf("  after 100 ms: out[0] %#010" B_PRIx32 ", out[1] %#010" B_PRIx32
		"\n", system[0], system[1]);
	FlushCpuCache(system, systemOutput.buf->size);
	printf("  after a CPU cache flush: out[0] %#010" B_PRIx32 ", out[1] %#010"
		B_PRIx32 "%s\n", system[0], system[1], CheckOutput(system, 1024,
			"flushed") ? ", all values OK (the GPU writes are not snooped)"
			: "");

	auto memMgr = gDevice.MemMgr().Switch();
	uint64 gpuAddress = systemOutput.buf->gpuPhysAdr;
	const uint64 *pageTable = (const uint64*)memMgr->GartPageTable().adr;
	printf("  output at %#" B_PRIx64 ", PTE %#018" B_PRIx64 "\n", gpuAddress,
		pageTable[(gpuAddress - memMgr->fGttRange.beg) / B_PAGE_SIZE]);
	PolarisInvalidateHdp();
	MappedBuffer dummy(memMgr->fDummyPage);
	if (dummy.adr != NULL) {
		volatile uint32 *words = (volatile uint32*)dummy.adr;
		printf("  dummy page: %#010" B_PRIx32 " %#010" B_PRIx32 " %#010"
			B_PRIx32 " %#010" B_PRIx32 "\n", words[0], words[1], words[2],
			words[3]);
	}
	gfx.PrintVmFaults();

	// what does the GPU see at out[0]: memory and TC L2, copied to VRAM
	vram[0] = vram[1] = 0xbad0bad0;
	PolarisFlushHdp();
	*fenceWord = 0;
	if (gfx.Begin(32) != B_OK)
		return;
	gfx.EmitCopyData(gpuAddress, vramOutput.buf->gpuPhysAdr, false);
	gfx.EmitCopyData(gpuAddress, vramOutput.buf->gpuPhysAdr + 4, true);
	gfx.EmitFence(fenceAddress, 0x4d, false);
	gfx.Commit();
	bigtime_t start = system_time();
	while (*fenceWord != 0x4d && system_time() - start < 1000000)
		snooze(100);
	PolarisInvalidateHdp();
	printf("  GPU view of out[0]: memory %#010" B_PRIx32 ", TC L2 %#010"
		B_PRIx32 "%s\n", vram[0], vram[1],
		*fenceWord == 0x4d ? "" : " (no fence)");

	// the CP through the TC L2: reads and writes of system memory
	system[2] = 0x11112222;
	FlushCpuCache(system, systemOutput.buf->size);
	vram[2] = vram[3] = 0xbad0bad0;
	PolarisFlushHdp();
	*fenceWord = 0;
	if (gfx.Begin(48) != B_OK)
		return;
	gfx.EmitCopyData(gpuAddress + 8, vramOutput.buf->gpuPhysAdr + 8, false);
	gfx.EmitCopyData(gpuAddress + 8, vramOutput.buf->gpuPhysAdr + 12, true);
	gfx.EmitWriteData(gpuAddress + 16, 0x33334444, false);
	gfx.EmitWriteData(gpuAddress + 20, 0x55556666, true);
	gfx.EmitFence(fenceAddress, 0x4e, false);
	gfx.Commit();
	start = system_time();
	while (*fenceWord != 0x4e && system_time() - start < 1000000)
		snooze(100);
	PolarisInvalidateHdp();
	printf("  CP read of 0x11112222 from GTT: memory %#010" B_PRIx32
		", TC L2 %#010" B_PRIx32 "%s\n", vram[2], vram[3],
		*fenceWord == 0x4e ? "" : " (no fence)");
	printf("  CP write to GTT: memory %#010" B_PRIx32 " (0x33334444), TC L2 %#010"
		B_PRIx32 " (0x55556666)\n", system[4], system[5]);
	gfx.PrintVmFaults();
}


static status_t
RunComputeTest(PolarisGfx &gfx, uint64 fenceAddress,
	volatile uint32 *fenceWord)
{
	const uint32 kGroups = 16, kCount = kGroups * 64;

	auto memMgr = gDevice.MemMgr().Switch();
	MappedBuffer emptyShader(memMgr->Alloc(boDomainVramMappable, B_PAGE_SIZE,
		B_PAGE_SIZE));
	MappedBuffer shader(memMgr->Alloc(boDomainVramMappable, B_PAGE_SIZE,
		B_PAGE_SIZE));
	MappedBuffer vramOutput(memMgr->Alloc(boDomainVramMappable, kCount * 4));
	MappedBuffer systemOutput(memMgr->Alloc(boDomainGtt, kCount * 4));
	if (emptyShader.adr == NULL || shader.adr == NULL
		|| vramOutput.adr == NULL || systemOutput.adr == NULL)
		return B_NO_MEMORY;
	memset(emptyShader.adr, 0, B_PAGE_SIZE);
	memcpy(emptyShader.adr, kEmptyShader, sizeof(kEmptyShader));
	memset(shader.adr, 0, B_PAGE_SIZE);
	memcpy(shader.adr, kFillShader, sizeof(kFillShader));
	memset(vramOutput.adr, 0, kCount * 4);
	memset(systemOutput.adr, 0, kCount * 4);
	PolarisFlushHdp();

	// 4a. waves start and end
	CheckRet(Dispatch(gfx, "empty shader", emptyShader.buf->gpuPhysAdr,
		vramOutput.buf->gpuPhysAdr, kCount * 4, kGroups, fenceAddress,
		fenceWord, 0x4a));
	printf("4a. empty shader (VRAM), %" B_PRIu32 " x 64 threads: OK\n",
		kGroups);

	// 4b. stores to VRAM
	CheckRet(Dispatch(gfx, "store to VRAM", shader.buf->gpuPhysAdr,
		vramOutput.buf->gpuPhysAdr, kCount * 4, kGroups, fenceAddress,
		fenceWord, 0x4b));
	PolarisInvalidateHdp();
	bool ok = CheckOutput((volatile uint32*)vramOutput.adr, kCount,
		"store to VRAM");
	printf("4b. buffer_store shader -> VRAM, %" B_PRIu32 " values: %s\n",
		kCount, ok ? "OK" : "[!] FAILED");
	if (!ok) {
		gfx.PrintVmFaults();
		return B_ERROR;
	}

	// 4c. stores to system memory through the GART, with no dirty CPU cache
	// lines for the output, so that a non-snooped GPU write is not lost
	FlushCpuCache(systemOutput.adr, kCount * 4);
	CheckRet(Dispatch(gfx, "store to GTT", shader.buf->gpuPhysAdr,
		systemOutput.buf->gpuPhysAdr, kCount * 4, kGroups, fenceAddress,
		fenceWord, 0x4c));
	ok = CheckOutput((volatile uint32*)systemOutput.adr, kCount,
		"store to GTT");
	printf("4c. buffer_store shader -> system memory, %" B_PRIu32
		" values: %s\n", kCount, ok ? "OK" : "[!] FAILED");
	if (!ok)
		DiagnoseGttStore(gfx, systemOutput, vramOutput, fenceAddress,
			fenceWord);
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

	gfx.SetupShaderMemory();
	return RunComputeTest(gfx, system.buf->gpuPhysAdr + 32, systemWords + 8);
}


status_t
PolarisGfxTest(bool allContexts)
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

	status = gDevice.MemMgr().Switch()->InitGartPolaris(allContexts);
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
