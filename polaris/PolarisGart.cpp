// GART (VM context 0) for Polaris, following Linux gmc_v8_0_mc_program() and
// gmc_v8_0_gart_enable(). The VBIOS memory controller setup (FB_LOCATION) is
// kept. Every register written here is saved first and restored by
// FiniGartPolaris().

#include "RadeonMemory.h"
#include "RadeonDevice.h"
#include "Radeon.h"

#include <stdio.h>
#include <string.h>
#include <OS.h>

#include "vi/gmc_8_1_d.h"
#include "vi/gmc_8_1_sh_mask.h"
#include "vi/oss_3_0_d.h"
#include "vi/oss_3_0_sh_mask.h"

#define CheckRet(err) {status_t _err = (err); if (_err < B_OK) return _err;}

#define SET_FIELD(value, reg, field, fieldValue) \
	(((value) & ~reg##__##field##_MASK) \
		| (((uint32)(fieldValue) << reg##__##field##__SHIFT) \
			& reg##__##field##_MASK))


// GART size: 512 MB of GPU address space above VRAM (1 MB page table)
static const uint64 kGartBase = 0xff00000000;
static const uint64 kGartSize = 512 * 1024 * 1024;
// 64 KB fragments, as Linux's default fragment_size
static const uint32 kFragmentSize = 4;

static const uint32 kSavedRegisters[] = {
	mmMC_VM_SYSTEM_APERTURE_LOW_ADDR,
	mmMC_VM_SYSTEM_APERTURE_HIGH_ADDR,
	mmMC_VM_SYSTEM_APERTURE_DEFAULT_ADDR,
	mmMC_VM_AGP_BASE,
	mmMC_VM_AGP_TOP,
	mmMC_VM_AGP_BOT,
	mmMC_VM_MX_L1_TLB_CNTL,
	mmVM_L2_CNTL,
	mmVM_L2_CNTL2,
	mmVM_L2_CNTL3,
	mmVM_L2_CNTL4,
	mmVM_CONTEXT0_PAGE_TABLE_START_ADDR,
	mmVM_CONTEXT0_PAGE_TABLE_END_ADDR,
	mmVM_CONTEXT0_PAGE_TABLE_BASE_ADDR,
	mmVM_CONTEXT0_PROTECTION_FAULT_DEFAULT_ADDR,
	mmVM_CONTEXT0_CNTL2,
	mmVM_CONTEXT0_CNTL,
	mmVM_L2_CONTEXT1_IDENTITY_APERTURE_LOW_ADDR,
	mmVM_L2_CONTEXT1_IDENTITY_APERTURE_HIGH_ADDR,
	mmVM_L2_CONTEXT_IDENTITY_PHYSICAL_OFFSET,
	mmVM_CONTEXT1_PAGE_TABLE_START_ADDR,
	mmVM_CONTEXT1_PAGE_TABLE_END_ADDR,
	mmVM_CONTEXT1_PAGE_TABLE_BASE_ADDR,
	mmVM_CONTEXT1_PAGE_TABLE_BASE_ADDR + 1,
	mmVM_CONTEXT1_PAGE_TABLE_BASE_ADDR + 2,
	mmVM_CONTEXT1_PAGE_TABLE_BASE_ADDR + 3,
	mmVM_CONTEXT1_PAGE_TABLE_BASE_ADDR + 4,
	mmVM_CONTEXT1_PAGE_TABLE_BASE_ADDR + 5,
	mmVM_CONTEXT1_PAGE_TABLE_BASE_ADDR + 6,
	mmVM_CONTEXT8_PAGE_TABLE_BASE_ADDR,
	mmVM_CONTEXT8_PAGE_TABLE_BASE_ADDR + 1,
	mmVM_CONTEXT8_PAGE_TABLE_BASE_ADDR + 2,
	mmVM_CONTEXT8_PAGE_TABLE_BASE_ADDR + 3,
	mmVM_CONTEXT8_PAGE_TABLE_BASE_ADDR + 4,
	mmVM_CONTEXT8_PAGE_TABLE_BASE_ADDR + 5,
	mmVM_CONTEXT8_PAGE_TABLE_BASE_ADDR + 6,
	mmVM_CONTEXT8_PAGE_TABLE_BASE_ADDR + 7,
	mmVM_CONTEXT1_PROTECTION_FAULT_DEFAULT_ADDR,
	mmVM_CONTEXT1_CNTL2,
	mmVM_CONTEXT1_CNTL,
};

static uint32 sSavedValues[B_COUNT_OF(kSavedRegisters)];


static void
WaitForMcIdle(const char *when)
{
	const uint32 busyMask = SRBM_STATUS__MCB_BUSY_MASK
		| SRBM_STATUS__MCB_NON_DISPLAY_BUSY_MASK | SRBM_STATUS__MCC_BUSY_MASK
		| SRBM_STATUS__MCD_BUSY_MASK | SRBM_STATUS__VMC_BUSY_MASK
		| SRBM_STATUS__VMC1_BUSY_MASK;
	uint32 status = 0;
	for (int i = 0; i < 1000; i++) {
		status = ReadReg4AmdGpu(mmSRBM_STATUS);
		if ((status & busyMask) == 0)
			return;
		snooze(1);
	}
	// as in Linux, only a warning: the display keeps the MC busy
	printf("  MC not idle %s (SRBM_STATUS %#010" B_PRIx32 ")\n", when, status);
}


status_t
MemoryManager::InitGartPolaris(bool vmContexts)
{
	if (fGartEnabled)
		return B_OK;
	if (!gDevice.RegsWritable()) {
		printf("[!] GART needs writable registers\n");
		return B_NOT_ALLOWED;
	}

	for (uint32 i = 0; i < B_COUNT_OF(kSavedRegisters); i++)
		sSavedValues[i] = ReadReg4AmdGpu(kSavedRegisters[i]);
	fGartRegistersSaved = true;

	fGttRange = {.beg = kGartBase, .size = kGartSize};
	fGartPageTable.SetTo(Alloc(boDomainVramMappable,
		fGttRange.size / B_PAGE_SIZE * sizeof(uint64)));
	if (fGartPageTable.adr == NULL)
		return B_NO_MEMORY;
	memset(fGartPageTable.adr, 0, fGartPageTable.buf->size);

	uint64 vramStart = fVramRange.beg;
	uint64 vramEnd = fVramRange.beg + fVramRange.size - 1;

	// *** gmc_v8_0_mc_program(), without FB_LOCATION and the VGA lockout
	// (radeon_hd already disables VGA memory and rendering)
	WaitForMcIdle("before aperture setup");
	WriteReg4AmdGpu(mmMC_VM_SYSTEM_APERTURE_LOW_ADDR, vramStart >> 12);
	WriteReg4AmdGpu(mmMC_VM_SYSTEM_APERTURE_HIGH_ADDR, vramEnd >> 12);
	WriteReg4AmdGpu(mmMC_VM_SYSTEM_APERTURE_DEFAULT_ADDR,
		fVramScratch->gpuPhysAdr >> 12);
	// AGP aperture off (start above end)
	WriteReg4AmdGpu(mmMC_VM_AGP_BASE, 0);
	WriteReg4AmdGpu(mmMC_VM_AGP_TOP, 0);
	WriteReg4AmdGpu(mmMC_VM_AGP_BOT, 0xffffffffffffULL >> 22);
	WaitForMcIdle("after aperture setup");

	// *** gmc_v8_0_gart_enable(), context 0 only
	uint32 value = ReadReg4AmdGpu(mmMC_VM_MX_L1_TLB_CNTL);
	value = SET_FIELD(value, MC_VM_MX_L1_TLB_CNTL, ENABLE_L1_TLB, 1);
	value = SET_FIELD(value, MC_VM_MX_L1_TLB_CNTL,
		ENABLE_L1_FRAGMENT_PROCESSING, 1);
	value = SET_FIELD(value, MC_VM_MX_L1_TLB_CNTL, SYSTEM_ACCESS_MODE, 3);
	value = SET_FIELD(value, MC_VM_MX_L1_TLB_CNTL,
		ENABLE_ADVANCED_DRIVER_MODEL, 1);
	value = SET_FIELD(value, MC_VM_MX_L1_TLB_CNTL,
		SYSTEM_APERTURE_UNMAPPED_ACCESS, 0);
	WriteReg4AmdGpu(mmMC_VM_MX_L1_TLB_CNTL, value);

	value = ReadReg4AmdGpu(mmVM_L2_CNTL);
	value = SET_FIELD(value, VM_L2_CNTL, ENABLE_L2_CACHE, 1);
	value = SET_FIELD(value, VM_L2_CNTL, ENABLE_L2_FRAGMENT_PROCESSING, 1);
	value = SET_FIELD(value, VM_L2_CNTL,
		ENABLE_L2_PTE_CACHE_LRU_UPDATE_BY_WRITE, 1);
	value = SET_FIELD(value, VM_L2_CNTL,
		ENABLE_L2_PDE0_CACHE_LRU_UPDATE_BY_WRITE, 1);
	value = SET_FIELD(value, VM_L2_CNTL, EFFECTIVE_L2_QUEUE_SIZE, 7);
	value = SET_FIELD(value, VM_L2_CNTL, CONTEXT1_IDENTITY_ACCESS_MODE, 1);
	value = SET_FIELD(value, VM_L2_CNTL,
		ENABLE_DEFAULT_PAGE_OUT_TO_SYSTEM_MEMORY, 1);
	WriteReg4AmdGpu(mmVM_L2_CNTL, value);

	value = ReadReg4AmdGpu(mmVM_L2_CNTL2);
	value = SET_FIELD(value, VM_L2_CNTL2, INVALIDATE_ALL_L1_TLBS, 1);
	value = SET_FIELD(value, VM_L2_CNTL2, INVALIDATE_L2_CACHE, 1);
	WriteReg4AmdGpu(mmVM_L2_CNTL2, value);

	value = ReadReg4AmdGpu(mmVM_L2_CNTL3);
	value = SET_FIELD(value, VM_L2_CNTL3, L2_CACHE_BIGK_ASSOCIATIVITY, 1);
	value = SET_FIELD(value, VM_L2_CNTL3, BANK_SELECT, kFragmentSize);
	value = SET_FIELD(value, VM_L2_CNTL3, L2_CACHE_BIGK_FRAGMENT_SIZE,
		kFragmentSize);
	WriteReg4AmdGpu(mmVM_L2_CNTL3, value);

	// page directory and tables in VRAM, not system memory
	value = ReadReg4AmdGpu(mmVM_L2_CNTL4);
	value &= ~(VM_L2_CNTL4__VMC_TAP_CONTEXT0_PDE_REQUEST_PHYSICAL_MASK
		| VM_L2_CNTL4__VMC_TAP_CONTEXT0_PDE_REQUEST_SHARED_MASK
		| VM_L2_CNTL4__VMC_TAP_CONTEXT0_PDE_REQUEST_SNOOP_MASK
		| VM_L2_CNTL4__VMC_TAP_CONTEXT0_PTE_REQUEST_PHYSICAL_MASK
		| VM_L2_CNTL4__VMC_TAP_CONTEXT0_PTE_REQUEST_SHARED_MASK
		| VM_L2_CNTL4__VMC_TAP_CONTEXT0_PTE_REQUEST_SNOOP_MASK
		| VM_L2_CNTL4__VMC_TAP_CONTEXT1_PDE_REQUEST_PHYSICAL_MASK
		| VM_L2_CNTL4__VMC_TAP_CONTEXT1_PDE_REQUEST_SHARED_MASK
		| VM_L2_CNTL4__VMC_TAP_CONTEXT1_PDE_REQUEST_SNOOP_MASK
		| VM_L2_CNTL4__VMC_TAP_CONTEXT1_PTE_REQUEST_PHYSICAL_MASK
		| VM_L2_CNTL4__VMC_TAP_CONTEXT1_PTE_REQUEST_SHARED_MASK
		| VM_L2_CNTL4__VMC_TAP_CONTEXT1_PTE_REQUEST_SNOOP_MASK);
	WriteReg4AmdGpu(mmVM_L2_CNTL4, value);

	WriteReg4AmdGpu(mmVM_CONTEXT0_PAGE_TABLE_START_ADDR, fGttRange.beg >> 12);
	WriteReg4AmdGpu(mmVM_CONTEXT0_PAGE_TABLE_END_ADDR,
		(fGttRange.beg + fGttRange.size - 1) >> 12);
	WriteReg4AmdGpu(mmVM_CONTEXT0_PAGE_TABLE_BASE_ADDR,
		fGartPageTable.buf->gpuPhysAdr >> 12);
	WriteReg4AmdGpu(mmVM_CONTEXT0_PROTECTION_FAULT_DEFAULT_ADDR,
		fDummyPage->gpuPhysAdr >> 12);
	WriteReg4AmdGpu(mmVM_CONTEXT0_CNTL2, 0);
	value = ReadReg4AmdGpu(mmVM_CONTEXT0_CNTL);
	value = SET_FIELD(value, VM_CONTEXT0_CNTL, ENABLE_CONTEXT, 1);
	value = SET_FIELD(value, VM_CONTEXT0_CNTL, PAGE_TABLE_DEPTH, 0);
	value = SET_FIELD(value, VM_CONTEXT0_CNTL,
		RANGE_PROTECTION_FAULT_ENABLE_DEFAULT, 1);
	WriteReg4AmdGpu(mmVM_CONTEXT0_CNTL, value);

	WriteReg4AmdGpu(mmVM_L2_CONTEXT1_IDENTITY_APERTURE_LOW_ADDR, 0);
	WriteReg4AmdGpu(mmVM_L2_CONTEXT1_IDENTITY_APERTURE_HIGH_ADDR, 0);
	WriteReg4AmdGpu(mmVM_L2_CONTEXT_IDENTITY_PHYSICAL_OFFSET, 0);

	// *** contexts 1-15: per process address spaces (AddressSpace), two
	// level page tables with 512 entry page tables, page directories set
	// per submission (WriteVmFlush()). Until then they point at an empty
	// page directory: every access faults to the dummy page.
	if (vmContexts) {
		MappedBuffer emptyDirectory(Alloc(boDomainVramMappable,
			AddressSpace::pageDirLen * sizeof(Pte)));
		if (emptyDirectory.adr == NULL)
			return B_NO_MEMORY;
		memset(emptyDirectory.adr, 0, emptyDirectory.buf->size);
		fEmptyPageDir = emptyDirectory.buf;

		WriteReg4AmdGpu(mmVM_CONTEXT1_PAGE_TABLE_START_ADDR, 0);
		WriteReg4AmdGpu(mmVM_CONTEXT1_PAGE_TABLE_END_ADDR,
			(uint32)AddressSpace::pageDirLen * AddressSpace::pageTableLen - 1);
		for (uint32 i = 1; i < 16; i++) {
			WriteReg4AmdGpu(i < 8 ? mmVM_CONTEXT0_PAGE_TABLE_BASE_ADDR + i
					: mmVM_CONTEXT8_PAGE_TABLE_BASE_ADDR + i - 8,
				fEmptyPageDir->gpuPhysAdr >> 12);
		}
		WriteReg4AmdGpu(mmVM_CONTEXT1_PROTECTION_FAULT_DEFAULT_ADDR,
			fDummyPage->gpuPhysAdr >> 12);
		WriteReg4AmdGpu(mmVM_CONTEXT1_CNTL2, 4);
		value = ReadReg4AmdGpu(mmVM_CONTEXT1_CNTL);
		value = SET_FIELD(value, VM_CONTEXT1_CNTL, ENABLE_CONTEXT, 1);
		value = SET_FIELD(value, VM_CONTEXT1_CNTL, PAGE_TABLE_DEPTH, 1);
		value = SET_FIELD(value, VM_CONTEXT1_CNTL,
			RANGE_PROTECTION_FAULT_ENABLE_DEFAULT, 1);
		value = SET_FIELD(value, VM_CONTEXT1_CNTL,
			DUMMY_PAGE_PROTECTION_FAULT_ENABLE_DEFAULT, 1);
		value = SET_FIELD(value, VM_CONTEXT1_CNTL,
			PDE0_PROTECTION_FAULT_ENABLE_DEFAULT, 1);
		value = SET_FIELD(value, VM_CONTEXT1_CNTL,
			VALID_PROTECTION_FAULT_ENABLE_DEFAULT, 1);
		value = SET_FIELD(value, VM_CONTEXT1_CNTL,
			READ_PROTECTION_FAULT_ENABLE_DEFAULT, 1);
		value = SET_FIELD(value, VM_CONTEXT1_CNTL,
			WRITE_PROTECTION_FAULT_ENABLE_DEFAULT, 1);
		value = SET_FIELD(value, VM_CONTEXT1_CNTL,
			EXECUTE_PROTECTION_FAULT_ENABLE_DEFAULT, 1);
		value = SET_FIELD(value, VM_CONTEXT1_CNTL, PAGE_TABLE_BLOCK_SIZE, 0);
		// gmc_v8_0_vm_fault_interrupt_state(): faults are reported through
		// the IH (source 146/147)
		value |= VM_CONTEXT1_CNTL__RANGE_PROTECTION_FAULT_ENABLE_INTERRUPT_MASK
			| VM_CONTEXT1_CNTL__DUMMY_PAGE_PROTECTION_FAULT_ENABLE_INTERRUPT_MASK
			| VM_CONTEXT1_CNTL__PDE0_PROTECTION_FAULT_ENABLE_INTERRUPT_MASK
			| VM_CONTEXT1_CNTL__VALID_PROTECTION_FAULT_ENABLE_INTERRUPT_MASK
			| VM_CONTEXT1_CNTL__READ_PROTECTION_FAULT_ENABLE_INTERRUPT_MASK
			| VM_CONTEXT1_CNTL__WRITE_PROTECTION_FAULT_ENABLE_INTERRUPT_MASK
			| VM_CONTEXT1_CNTL__EXECUTE_PROTECTION_FAULT_ENABLE_INTERRUPT_MASK;
		WriteReg4AmdGpu(mmVM_CONTEXT1_CNTL, value);
		printf("VM:        contexts 1-15 enabled, %" B_PRIu64 " GB each\n",
			(uint64)AddressSpace::pageDirLen * AddressSpace::pageTableLen
				* B_PAGE_SIZE >> 30);
	}

	// clear fault status left from earlier runs (Linux gmc_v8_0_process_interrupt())
	WriteReg4AmdGpu(mmVM_CONTEXT0_CNTL2,
		ReadReg4AmdGpu(mmVM_CONTEXT0_CNTL2) | 1);
	WriteReg4AmdGpu(mmVM_CONTEXT1_CNTL2,
		ReadReg4AmdGpu(mmVM_CONTEXT1_CNTL2) | 1);
	snooze(10);
	WriteReg4AmdGpu(mmVM_CONTEXT0_CNTL2,
		ReadReg4AmdGpu(mmVM_CONTEXT0_CNTL2) & ~1u);
	WriteReg4AmdGpu(mmVM_CONTEXT1_CNTL2,
		ReadReg4AmdGpu(mmVM_CONTEXT1_CNTL2) & ~1u);
	printf("VM:        fault status context 0 %#" B_PRIx32 ", context 1 %#"
		B_PRIx32 "\n", ReadReg4AmdGpu(mmVM_CONTEXT0_PROTECTION_FAULT_STATUS),
		ReadReg4AmdGpu(mmVM_CONTEXT1_PROTECTION_FAULT_STATUS));

	GartFlushTlb();
	fGartEnabled = true;
	fDomainPools[boDomainGtt].Register(fGttRange.beg, fGttRange.size);

	// fences and read pointers written back by the engines
	fWritebackBuf.SetTo(Alloc(boDomainGtt, B_PAGE_SIZE));
	if (fWritebackBuf.adr == NULL)
		return B_NO_MEMORY;
	memset(fWritebackBuf.adr, 0, B_PAGE_SIZE);
	fWritebackPool.Register(fWritebackBuf.buf->gpuPhysAdr,
		fWritebackBuf.buf->size);

	printf("GART:      %#" B_PRIx64 " - %#" B_PRIx64 " (%" B_PRIu64
		" MB), page table at %#" B_PRIx64 "\n", fGttRange.beg,
		fGttRange.beg + fGttRange.size - 1, fGttRange.size / (1024 * 1024),
		fGartPageTable.buf->gpuPhysAdr);
	return B_OK;
}


void
MemoryManager::FiniGartPolaris()
{
	if (!fGartRegistersSaved)
		return;

	// gmc_v8_0_gart_disable(), then the VBIOS values
	WriteReg4AmdGpu(mmVM_CONTEXT0_CNTL, 0);
	WriteReg4AmdGpu(mmVM_CONTEXT1_CNTL, 0);
	uint32 value = ReadReg4AmdGpu(mmMC_VM_MX_L1_TLB_CNTL);
	value = SET_FIELD(value, MC_VM_MX_L1_TLB_CNTL, ENABLE_L1_TLB, 0);
	value = SET_FIELD(value, MC_VM_MX_L1_TLB_CNTL,
		ENABLE_L1_FRAGMENT_PROCESSING, 0);
	value = SET_FIELD(value, MC_VM_MX_L1_TLB_CNTL,
		ENABLE_ADVANCED_DRIVER_MODEL, 0);
	WriteReg4AmdGpu(mmMC_VM_MX_L1_TLB_CNTL, value);
	value = ReadReg4AmdGpu(mmVM_L2_CNTL);
	value = SET_FIELD(value, VM_L2_CNTL, ENABLE_L2_CACHE, 0);
	WriteReg4AmdGpu(mmVM_L2_CNTL, value);
	WriteReg4AmdGpu(mmVM_L2_CNTL2, 0);

	WaitForMcIdle("before restoring the VBIOS setup");
	for (int32 i = B_COUNT_OF(kSavedRegisters) - 1; i >= 0; i--)
		WriteReg4AmdGpu(kSavedRegisters[i], sSavedValues[i]);
	WaitForMcIdle("after restoring the VBIOS setup");

	bool restored = true;
	for (uint32 i = 0; i < B_COUNT_OF(kSavedRegisters); i++) {
		uint32 now = ReadReg4AmdGpu(kSavedRegisters[i]);
		if (now != sSavedValues[i]) {
			printf("  [!] register %#" B_PRIx32 " is %#010" B_PRIx32
				", was %#010" B_PRIx32 "\n", kSavedRegisters[i], now,
				sSavedValues[i]);
			restored = false;
		}
	}
	printf("GART disabled, %s\n", restored
		? "VBIOS register values restored" : "[!] restore incomplete");

	fGartEnabled = false;
	fGartRegistersSaved = false;
}
