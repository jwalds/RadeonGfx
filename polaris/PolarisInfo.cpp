// Read-only probe of a Polaris GPU through the radeon_hd render device.
// Only reads registers: no BIOS tables, no writes.

#include "PolarisInfo.h"
#include "RenderDevice.h"

#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <OS.h>
#include <String.h>
#include <private/shared/AutoDeleter.h>
#include <private/shared/AutoDeleterOS.h>
#include <private/shared/AutoDeleterPosix.h>

#include "vi/gfx_8_0_d.h"
#include "vi/gfx_8_0_sh_mask.h"
#include "vi/gmc_8_1_d.h"
#include "vi/gmc_8_1_sh_mask.h"
#include "vi/oss_3_0_d.h"
#include "vi/oss_3_0_sh_mask.h"
#include "vi/bif_5_0_d.h"

#define CheckRet(err) {status_t _err = (err); if (_err < B_OK) return _err;}

#define FIELD(value, reg, field) \
	(((value) & reg##__##field##_MASK) >> reg##__##field##__SHIFT)


class RegisterReader {
public:
	RegisterReader(const volatile uint32 *regs, uint64 size):
		fRegs(regs), fCount(size / 4) {}

	uint32 Read(uint32 index) const
	{
		if (index >= fCount)
			return 0xffffffff;
		return fRegs[index];
	}

private:
	const volatile uint32 *fRegs;
	uint64 fCount;
};


static void
PrintEngineState(const RegisterReader &regs)
{
	uint32 grbmStatus = regs.Read(mmGRBM_STATUS);
	uint32 cpMeCntl = regs.Read(mmCP_ME_CNTL);
	uint32 cpMecCntl = regs.Read(mmCP_MEC_CNTL);
	uint32 rlcCntl = regs.Read(mmRLC_CNTL);
	uint32 sdma0Cntl = regs.Read(mmSDMA0_F32_CNTL);
	uint32 sdma1Cntl = regs.Read(mmSDMA1_F32_CNTL);
	uint32 ihRbCntl = regs.Read(mmIH_RB_CNTL);
	uint32 vmL2Cntl = regs.Read(mmVM_L2_CNTL);
	uint32 vmContext0Cntl = regs.Read(mmVM_CONTEXT0_CNTL);

	printf("Engines\n");
	printf("  GRBM_STATUS        %#010" B_PRIx32 " (GUI %s)\n", grbmStatus,
		FIELD(grbmStatus, GRBM_STATUS, GUI_ACTIVE) ? "active" : "idle");
	printf("  GRBM_STATUS2       %#010" B_PRIx32 "\n",
		regs.Read(mmGRBM_STATUS2));
	printf("  SRBM_STATUS        %#010" B_PRIx32 "\n", regs.Read(mmSRBM_STATUS));
	printf("  SRBM_STATUS2       %#010" B_PRIx32 "\n",
		regs.Read(mmSRBM_STATUS2));
	printf("  CP_ME_CNTL         %#010" B_PRIx32 " (ME %s, PFP %s, CE %s)\n",
		cpMeCntl,
		FIELD(cpMeCntl, CP_ME_CNTL, ME_HALT) ? "halted" : "running",
		FIELD(cpMeCntl, CP_ME_CNTL, PFP_HALT) ? "halted" : "running",
		FIELD(cpMeCntl, CP_ME_CNTL, CE_HALT) ? "halted" : "running");
	printf("  CP_MEC_CNTL        %#010" B_PRIx32 " (MEC1 %s)\n", cpMecCntl,
		FIELD(cpMecCntl, CP_MEC_CNTL, MEC_ME1_HALT) ? "halted" : "running");
	printf("  RLC_CNTL           %#010" B_PRIx32 " (RLC %s)\n", rlcCntl,
		FIELD(rlcCntl, RLC_CNTL, RLC_ENABLE_F32) ? "enabled" : "disabled");
	printf("  SDMA0_F32_CNTL     %#010" B_PRIx32 " (%s)\n", sdma0Cntl,
		FIELD(sdma0Cntl, SDMA0_F32_CNTL, HALT) ? "halted" : "running");
	printf("  SDMA1_F32_CNTL     %#010" B_PRIx32 " (%s)\n", sdma1Cntl,
		(sdma1Cntl & SDMA0_F32_CNTL__HALT_MASK) ? "halted" : "running");
	printf("  IH_RB_CNTL         %#010" B_PRIx32 " (ring %s)\n", ihRbCntl,
		FIELD(ihRbCntl, IH_RB_CNTL, RB_ENABLE) ? "enabled" : "disabled");
	printf("  VM_L2_CNTL         %#010" B_PRIx32 " (L2 %s)\n", vmL2Cntl,
		FIELD(vmL2Cntl, VM_L2_CNTL, ENABLE_L2_CACHE) ? "enabled" : "disabled");
	printf("  VM_CONTEXT0_CNTL   %#010" B_PRIx32 " (context 0 %s)\n",
		vmContext0Cntl,
		FIELD(vmContext0Cntl, VM_CONTEXT0_CNTL, ENABLE_CONTEXT)
			? "enabled" : "disabled");

	// command processor position and VM faults, for a hung GPU
	printf("  CP_STAT            %#010" B_PRIx32 "\n", regs.Read(mmCP_STAT));
	printf("  CP_CPF_STATUS      %#010" B_PRIx32 ", CP_CPF_BUSY_STAT %#010"
		B_PRIx32 "\n", regs.Read(mmCP_CPF_STATUS),
		regs.Read(mmCP_CPF_BUSY_STAT));
	printf("  CP_RB0 rptr %#" B_PRIx32 ", wptr %#" B_PRIx32 "\n",
		regs.Read(mmCP_RB0_RPTR), regs.Read(mmCP_RB0_WPTR));
	printf("  CP_IB1 base %#" B_PRIx32 "%08" B_PRIx32 ", %" B_PRIu32
		" dwords left; CP_IB2 base %#" B_PRIx32 "%08" B_PRIx32 ", %" B_PRIu32
		" dwords left\n", regs.Read(mmCP_IB1_BASE_HI),
		regs.Read(mmCP_IB1_BASE_LO), regs.Read(mmCP_IB1_BUFSZ),
		regs.Read(mmCP_IB2_BASE_HI), regs.Read(mmCP_IB2_BASE_LO),
		regs.Read(mmCP_IB2_BUFSZ));
	printf("  CP_STALLED_STAT1 %#010" B_PRIx32 ", STAT2 %#010" B_PRIx32
		", STAT3 %#010" B_PRIx32 "\n", regs.Read(mmCP_STALLED_STAT1),
		regs.Read(mmCP_STALLED_STAT2), regs.Read(mmCP_STALLED_STAT3));
	printf("  CP_PFP_HEADER_DUMP %#010" B_PRIx32 ", CP_ME_HEADER_DUMP %#010"
		B_PRIx32 ", CP_CE_HEADER_DUMP %#010" B_PRIx32 "\n",
		regs.Read(mmCP_PFP_HEADER_DUMP), regs.Read(mmCP_ME_HEADER_DUMP),
		regs.Read(mmCP_CE_HEADER_DUMP));
	printf("  VM_CONTEXT1_PROTECTION_FAULT_STATUS %#010" B_PRIx32 ", ADDR %#"
		B_PRIx32 "\n", regs.Read(mmVM_CONTEXT1_PROTECTION_FAULT_STATUS),
		regs.Read(mmVM_CONTEXT1_PROTECTION_FAULT_ADDR));
}


static void
PrintGfxConfig(const RegisterReader &regs)
{
	uint32 addrConfig = regs.Read(mmGB_ADDR_CONFIG);
	uint32 shaderArrayConfig = regs.Read(mmCC_GC_SHADER_ARRAY_CONFIG);
	uint32 userShaderArrayConfig = regs.Read(mmGC_USER_SHADER_ARRAY_CONFIG);
	uint32 rbDisable = regs.Read(mmCC_RB_BACKEND_DISABLE);
	uint32 userRbDisable = regs.Read(mmGC_USER_RB_BACKEND_DISABLE);

	printf("Graphics configuration\n");
	printf("  GB_ADDR_CONFIG     %#010" B_PRIx32 "\n", addrConfig);
	printf("    pipes %u, shader engines %u, pipe interleave %u bytes, "
		"row size %u KB\n",
		1u << FIELD(addrConfig, GB_ADDR_CONFIG, NUM_PIPES),
		(unsigned)FIELD(addrConfig, GB_ADDR_CONFIG, NUM_SHADER_ENGINES) + 1,
		256u << FIELD(addrConfig, GB_ADDR_CONFIG, PIPE_INTERLEAVE_SIZE),
		1u << FIELD(addrConfig, GB_ADDR_CONFIG, ROW_SIZE));

	// read through the broadcast index: shader engine 0, shader array 0
	uint32 inactiveCus
		= FIELD(shaderArrayConfig, CC_GC_SHADER_ARRAY_CONFIG, INACTIVE_CUS)
		| FIELD(userShaderArrayConfig, CC_GC_SHADER_ARRAY_CONFIG,
			INACTIVE_CUS);
	printf("  CC/GC_USER_SHADER_ARRAY_CONFIG %#010" B_PRIx32 " / %#010"
		B_PRIx32 "\n", shaderArrayConfig, userShaderArrayConfig);
	printf("    inactive CU mask (SE0 SH0): %#06" B_PRIx32 "\n", inactiveCus);
	uint32 rbMask
		= FIELD(rbDisable, CC_RB_BACKEND_DISABLE, BACKEND_DISABLE)
		| FIELD(userRbDisable, CC_RB_BACKEND_DISABLE, BACKEND_DISABLE);
	printf("  CC/GC_USER_RB_BACKEND_DISABLE  %#010" B_PRIx32 " / %#010"
		B_PRIx32 "\n", rbDisable, userRbDisable);
	printf("    disabled render backend mask: %#04" B_PRIx32 "\n", rbMask);
}


static void
PrintMemoryConfig(const RegisterReader &regs)
{
	uint32 fbLocation = regs.Read(mmMC_VM_FB_LOCATION);
	uint64 fbBase = (uint64)FIELD(fbLocation, MC_VM_FB_LOCATION, FB_BASE) << 24;
	uint64 fbTop = ((uint64)FIELD(fbLocation, MC_VM_FB_LOCATION, FB_TOP) << 24)
		| 0xffffff;

	printf("Memory controller\n");
	printf("  CONFIG_MEMSIZE     %" B_PRIu32 " MB\n",
		regs.Read(mmCONFIG_MEMSIZE));
	printf("  MC_VM_FB_LOCATION  %#010" B_PRIx32 " (VRAM at %#" B_PRIx64
		" - %#" B_PRIx64 ")\n", fbLocation, fbBase, fbTop);
	printf("  MC_VM_FB_OFFSET    %#010" B_PRIx32 "\n",
		regs.Read(mmMC_VM_FB_OFFSET));
	printf("  MC_VM_AGP_BASE/BOT/TOP %#" B_PRIx32 " %#" B_PRIx32 " %#"
		B_PRIx32 "\n", regs.Read(mmMC_VM_AGP_BASE), regs.Read(mmMC_VM_AGP_BOT),
		regs.Read(mmMC_VM_AGP_TOP));
	printf("  system aperture    %#" B_PRIx64 " - %#" B_PRIx64 "\n",
		(uint64)regs.Read(mmMC_VM_SYSTEM_APERTURE_LOW_ADDR) << 12,
		(uint64)regs.Read(mmMC_VM_SYSTEM_APERTURE_HIGH_ADDR) << 12);
	printf("  MC_ARB_RAMCFG      %#010" B_PRIx32 "\n",
		regs.Read(mmMC_ARB_RAMCFG));
	printf("  MC_SEQ_MISC0       %#010" B_PRIx32 " (memory type %" B_PRIu32
		")\n", regs.Read(mmMC_SEQ_MISC0), regs.Read(mmMC_SEQ_MISC0) >> 28);
	printf("  MC_SHARED_CHMAP    %#010" B_PRIx32 "\n",
		regs.Read(mmMC_SHARED_CHMAP));
}


status_t
PolarisInfo()
{
	FileDescriptorCloser fd;
	BString path;
	if (OpenRenderDevice(fd, path) < B_OK) {
		printf("no radeon_hd render device found in /dev/graphics\n");
		return B_ENTRY_NOT_FOUND;
	}
	printf("Device             %s\n", path.String());

	radeon_hd_gpu_info info;
	if (GetGpuInfo(fd.Get(), info) < B_OK) {
		printf("RADEON_GET_GPU_INFO failed\n");
		return B_ERROR;
	}

	printf("PCI ID             1002:%04x rev %#04x\n", info.pci_id,
		info.pci_revision);
	printf("DCE                %u.%u\n", info.dce_major, info.dce_minor);
	printf("VRAM               %" B_PRIu64 " MB, %" B_PRIu64
		" MB visible at %#" B_PRIx64 "\n",
		info.graphics_memory_size / (1024 * 1024),
		info.frame_buffer_size / (1024 * 1024), info.frame_buffer_phys);
	printf("Registers          area %" B_PRId32 ", %" B_PRIu64 " KB\n",
		info.registers_area, info.registers_size / 1024);
	printf("AtomBIOS           area %" B_PRId32 ", %" B_PRIu32 " KB\n",
		info.rom_area, info.rom_size / 1024);

	// read-only mapping: this probe must never write a register
	void *address = NULL;
	AreaDeleter regsArea(clone_area("radeon hd regs (read only)", &address,
		B_ANY_ADDRESS, B_READ_AREA, info.registers_area));
	if (!regsArea.IsSet()) {
		printf("can't clone register area: %s\n", strerror(regsArea.Get()));
		return regsArea.Get();
	}
	RegisterReader regs((const volatile uint32*)address, info.registers_size);

	printf("\n");
	PrintEngineState(regs);
	printf("\n");
	PrintGfxConfig(regs);
	printf("\n");
	PrintMemoryConfig(regs);
	return B_OK;
}


/*!	Reads the registers of a list ("<name> <dword offset>" per line, as
	docs/perf/reglist.txt) and prints "<name> <offset> <value>", the format
	of the Linux dump through amdgpu's debugfs, for comparing the two.
*/
status_t
PolarisRegs(const char *listPath)
{
	FILE *list = fopen(listPath, "r");
	if (list == NULL) {
		printf("can't open %s\n", listPath);
		return B_ENTRY_NOT_FOUND;
	}
	FileDescriptorCloser fd;
	BString path;
	radeon_hd_gpu_info info;
	if (OpenRenderDevice(fd, path) < B_OK || GetGpuInfo(fd.Get(), info) < B_OK) {
		fclose(list);
		printf("no radeon_hd render device\n");
		return B_ERROR;
	}
	void *address = NULL;
	AreaDeleter regsArea(clone_area("radeon hd regs (read only)", &address,
		B_ANY_ADDRESS, B_READ_AREA, info.registers_area));
	if (!regsArea.IsSet()) {
		fclose(list);
		return regsArea.Get();
	}
	RegisterReader regs((const volatile uint32*)address, info.registers_size);

	char name[128];
	unsigned int offset;
	while (fscanf(list, "%127s %x", name, &offset) == 2)
		printf("%s %#x %#010" B_PRIx32 "\n", name, offset, regs.Read(offset));
	fclose(list);
	return B_OK;
}
