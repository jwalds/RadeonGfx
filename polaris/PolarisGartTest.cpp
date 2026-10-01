// "RadeonGfx garttest": enable the GART (VM context 0), map a system memory
// buffer, check the page table, then restore the VBIOS setup. This is the
// first step that writes GPU registers. No engine uses the GART yet.

#include "PolarisGartTest.h"
#include "RenderDevice.h"
#include "RadeonDevice.h"
#include "RadeonMemory.h"
#include "Radeon.h"
#include "Poke.h"

#include <stdio.h>
#include <string.h>
#include <OS.h>

#include "vi/gmc_8_1_d.h"

#define CheckRet(err) {status_t _err = (err); if (_err < B_OK) return _err;}


static void
PrintVmRegisters(const char *title)
{
	static const struct {
		uint32 index;
		const char *name;
	} kRegisters[] = {
		{mmMC_VM_SYSTEM_APERTURE_LOW_ADDR, "SYSTEM_APERTURE_LOW"},
		{mmMC_VM_SYSTEM_APERTURE_HIGH_ADDR, "SYSTEM_APERTURE_HIGH"},
		{mmMC_VM_SYSTEM_APERTURE_DEFAULT_ADDR, "SYSTEM_APERTURE_DEFAULT"},
		{mmMC_VM_AGP_BOT, "AGP_BOT"},
		{mmMC_VM_MX_L1_TLB_CNTL, "MC_VM_MX_L1_TLB_CNTL"},
		{mmVM_L2_CNTL, "VM_L2_CNTL"},
		{mmVM_L2_CNTL3, "VM_L2_CNTL3"},
		{mmVM_L2_STATUS, "VM_L2_STATUS"},
		{mmVM_CONTEXT0_CNTL, "VM_CONTEXT0_CNTL"},
		{mmVM_CONTEXT0_PAGE_TABLE_START_ADDR, "CONTEXT0_PT_START"},
		{mmVM_CONTEXT0_PAGE_TABLE_END_ADDR, "CONTEXT0_PT_END"},
		{mmVM_CONTEXT0_PAGE_TABLE_BASE_ADDR, "CONTEXT0_PT_BASE"},
		{mmVM_CONTEXT1_PROTECTION_FAULT_STATUS, "PROTECTION_FAULT_STATUS"},
		{mmVM_INVALIDATE_RESPONSE, "VM_INVALIDATE_RESPONSE"},
	};
	printf("%s\n", title);
	for (uint32 i = 0; i < B_COUNT_OF(kRegisters); i++) {
		printf("  %-24s %#010" B_PRIx32 "\n", kRegisters[i].name,
			ReadReg4AmdGpu(kRegisters[i].index));
	}
}


static status_t
CheckMapping(MemoryManager &memMgr, BReference<BufferObject> buffer)
{
	MappedBuffer cpu(buffer);
	if (cpu.adr == NULL)
		return B_ERROR;
	memset(cpu.adr, 0x3c, buffer->size);

	const uint64 *pageTable = (const uint64*)memMgr.GartPageTable().adr;
	uint32 bad = 0;
	for (uint64 offset = 0; offset < buffer->size; offset += B_PAGE_SIZE) {
		uint64 physical;
		CheckRet(gPoke.GetPhysicalAddress(physical, (uint8*)cpu.adr + offset,
			B_PAGE_SIZE));
		uint64 pte = pageTable[(buffer->gpuPhysAdr - memMgr.fGttRange.beg
			+ offset) / B_PAGE_SIZE];
		uint64 expected = (physical & ~(uint64)(B_PAGE_SIZE - 1))
			| R600_PTE_VALID | R600_PTE_SYSTEM | R600_PTE_SNOOPED
			| R600_PTE_READABLE | R600_PTE_WRITEABLE;
		if (offset == 0) {
			printf("  first page: GPU %#" B_PRIx64 " -> physical %#" B_PRIx64
				", PTE %#018" B_PRIx64 "\n", buffer->gpuPhysAdr, physical,
				pte);
		}
		if (pte != expected)
			bad++;
	}
	printf("  %" B_PRIu64 " pages mapped, %" B_PRIu32 " bad PTEs\n",
		buffer->size / B_PAGE_SIZE, bad);
	return bad == 0 ? B_OK : B_ERROR;
}


status_t
PolarisGartTest()
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

	auto memMgr = gDevice.MemMgr().Switch();
	CheckRet(memMgr->InitPolaris());

	PrintVmRegisters("VM registers before (VBIOS)");
	status = memMgr->InitGartPolaris();
	if (status < B_OK) {
		memMgr->FiniGartPolaris();
		return status;
	}
	PrintVmRegisters("VM registers with the GART enabled");

	{
		printf("GTT buffer (system memory through the GART)\n");
		BReference<BufferObject> buffer = memMgr->Alloc(boDomainGtt,
			256 * 1024);
		if (!buffer.IsSet()) {
			printf("  [!] GTT allocation failed\n");
			status = B_NO_MEMORY;
		} else
			status = CheckMapping(*memMgr, buffer);
	}

	printf("holding for 5 seconds, the screen should stay normal\n");
	snooze(5000000);

	memMgr->FiniGartPolaris();
	PrintVmRegisters("VM registers after restoring");
	return status;
}
