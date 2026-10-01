#include "PolarisGfx.h"
#include "PolarisSdma.h"
#include "RadeonDevice.h"
#include "Radeon.h"

#include <stdio.h>
#include <string.h>
#include <OS.h>

#include "vi/gfx_8_0_d.h"
#include "vi/oss_3_0_d.h"
#include "vi/oss_3_0_sh_mask.h"
#include "vi/gmc_8_1_d.h"
#include "vi/gmc_8_1_sh_mask.h"
#include "vi/gfx_8_0_sh_mask.h"
#include "vi/vid.h"
#include "vi/clearstate_vi.h"

// gfx_8_0_enum.h
#define CACHE_FLUSH_AND_INV_TS_EVENT	0x14
#define CS_PARTIAL_FLUSH				0x7
static const uint32 kMtypeUc = 3;
static const uint32 kShMemAlignmentModeUnaligned = 3;

#define CheckRet(err) {status_t _err = (err); if (_err < B_OK) return _err;}

#define SET_FIELD(value, reg, field, fieldValue) \
	(((value) & ~reg##__##field##_MASK) \
		| (((uint32)(fieldValue) << reg##__##field##__SHIFT) \
			& reg##__##field##_MASK))

// Polaris 11 values from Linux gfx_v8_0.c
static const uint32 kMaxHwContexts = 8;
static const uint32 kRasterConfig = 0x16000012;
static const uint32 kRasterConfig1 = 0x00000000;

// golden_settings_polaris11_a11 and polaris11_golden_common_all:
// register, mask, value (VI: value is ORed in unmasked)
static const uint32 kGoldenSettings[] = {
	mmCB_HW_CONTROL, 0x0000f3cf, 0x00007208,
	mmCB_HW_CONTROL_2, 0x0f000000, 0x0f000000,
	mmCB_HW_CONTROL_3, 0x000001ff, 0x00000040,
	mmDB_DEBUG2, 0xf00fffff, 0x00000400,
	mmPA_SC_ENHANCE, 0xffffffff, 0x20000001,
	mmPA_SC_LINE_STIPPLE_STATE, 0x0000ff0f, 0x00000000,
	mmPA_SC_RASTER_CONFIG, 0x3f3fffff, 0x16000012,
	mmPA_SC_RASTER_CONFIG_1, 0x0000003f, 0x00000000,
	mmRLC_CGCG_CGLS_CTRL, 0x00000003, 0x0001003c,
	mmRLC_CGCG_CGLS_CTRL_3D, 0xffffffff, 0x0001003c,
	mmSQ_CONFIG, 0x07f80000, 0x01180000,
	mmTA_CNTL_AUX, 0x000f000f, 0x000b0000,
	mmTCC_CTRL, 0x00100000, 0xf31fff7f,
	mmTCP_ADDR_CONFIG, 0x000003ff, 0x000000f3,
	mmTCP_CHAN_STEER_HI, 0xffffffff, 0x00000000,
	mmTCP_CHAN_STEER_LO, 0xffffffff, 0x00003210,
	mmVGT_RESET_DEBUG, 0x00000004, 0x00000004,

	mmGRBM_GFX_INDEX, 0xffffffff, 0xe0000000,
	mmGB_ADDR_CONFIG, 0xffffffff, 0x22011002,
	mmSPI_RESOURCE_RESERVE_CU_0, 0xffffffff, 0x00000800,
	mmSPI_RESOURCE_RESERVE_CU_1, 0xffffffff, 0x00000800,
	mmSPI_RESOURCE_RESERVE_EN_CU_0, 0xffffffff, 0x00FF7FBF,
	mmSPI_RESOURCE_RESERVE_EN_CU_1, 0xffffffff, 0x00FF7FAF,
};

static const uint32 kSavedRegisters[] = {
	// golden settings
	mmCB_HW_CONTROL, mmCB_HW_CONTROL_2, mmCB_HW_CONTROL_3, mmDB_DEBUG2,
	mmPA_SC_ENHANCE, mmPA_SC_LINE_STIPPLE_STATE, mmPA_SC_RASTER_CONFIG,
	mmPA_SC_RASTER_CONFIG_1, mmRLC_CGCG_CGLS_CTRL, mmRLC_CGCG_CGLS_CTRL_3D,
	mmSQ_CONFIG, mmTA_CNTL_AUX, mmTCC_CTRL, mmTCP_ADDR_CONFIG,
	mmTCP_CHAN_STEER_HI, mmTCP_CHAN_STEER_LO, mmVGT_RESET_DEBUG,
	mmGB_ADDR_CONFIG, mmSPI_RESOURCE_RESERVE_CU_0,
	mmSPI_RESOURCE_RESERVE_CU_1, mmSPI_RESOURCE_RESERVE_EN_CU_0,
	mmSPI_RESOURCE_RESERVE_EN_CU_1,
	// command processor
	mmCP_RB_WPTR_DELAY, mmCP_RB_VMID, mmCP_RB0_RPTR_ADDR,
	mmCP_RB0_RPTR_ADDR_HI, mmCP_RB_WPTR_POLL_ADDR_LO,
	mmCP_RB_WPTR_POLL_ADDR_HI, mmCP_RB0_BASE, mmCP_RB0_BASE_HI,
	mmCP_RB_DOORBELL_CONTROL, mmCP_MAX_CONTEXT, mmCP_ENDIAN_SWAP,
	mmCP_DEVICE_ID, mmCP_RB0_CNTL,
	// shader memory (VMID 0, SRBM_GFX_CNTL selects VMID 0)
	mmSH_STATIC_MEM_CONFIG, mmSH_MEM_CONFIG, mmSH_MEM_BASES,
	mmSH_MEM_APE1_BASE, mmSH_MEM_APE1_LIMIT,
	// restored last
	mmGRBM_GFX_INDEX,
};

static uint32 sSavedValues[B_COUNT_OF(kSavedRegisters)];


static uint32
OrderBase2(uint32 value)
{
	uint32 order = 0;
	while ((1u << order) < value)
		order++;
	return order;
}


static void
HaltCp()
{
	// gfx_v8_0_cp_gfx_enable(false), compute (MEC) halted as well
	uint32 value = ReadReg4AmdGpu(mmCP_ME_CNTL);
	value = SET_FIELD(value, CP_ME_CNTL, ME_HALT, 1);
	value = SET_FIELD(value, CP_ME_CNTL, PFP_HALT, 1);
	value = SET_FIELD(value, CP_ME_CNTL, CE_HALT, 1);
	WriteReg4AmdGpu(mmCP_ME_CNTL, value);
	WriteReg4AmdGpu(mmCP_MEC_CNTL, CP_MEC_CNTL__MEC_ME1_HALT_MASK
		| CP_MEC_CNTL__MEC_ME2_HALT_MASK);
	snooze(50);
}


static void __attribute__((unused))
SoftReset()
{
	// gfx_v8_0_soft_reset(): reset RLC, GFX and the CP parts so a restart
	// doesn't see state from an earlier run; the firmware stays loaded
	const uint32 grbmReset = GRBM_SOFT_RESET__SOFT_RESET_RLC_MASK
		| GRBM_SOFT_RESET__SOFT_RESET_GFX_MASK
		| GRBM_SOFT_RESET__SOFT_RESET_CP_MASK
		| GRBM_SOFT_RESET__SOFT_RESET_CPF_MASK
		| GRBM_SOFT_RESET__SOFT_RESET_CPC_MASK
		| GRBM_SOFT_RESET__SOFT_RESET_CPG_MASK;
	const uint32 srbmReset = SRBM_SOFT_RESET__SOFT_RESET_GRBM_MASK
		| SRBM_SOFT_RESET__SOFT_RESET_SEM_MASK;

	uint32 value = ReadReg4AmdGpu(mmGMCON_DEBUG);
	WriteReg4AmdGpu(mmGMCON_DEBUG, value | GMCON_DEBUG__GFX_STALL_MASK
		| GMCON_DEBUG__GFX_CLEAR_MASK);
	snooze(100);

	value = ReadReg4AmdGpu(mmGRBM_SOFT_RESET);
	WriteReg4AmdGpu(mmGRBM_SOFT_RESET, value | grbmReset);
	ReadReg4AmdGpu(mmGRBM_SOFT_RESET);
	snooze(100);
	WriteReg4AmdGpu(mmGRBM_SOFT_RESET, value & ~grbmReset);
	ReadReg4AmdGpu(mmGRBM_SOFT_RESET);
	snooze(100);

	value = ReadReg4AmdGpu(mmSRBM_SOFT_RESET);
	WriteReg4AmdGpu(mmSRBM_SOFT_RESET, value | srbmReset);
	ReadReg4AmdGpu(mmSRBM_SOFT_RESET);
	snooze(100);
	WriteReg4AmdGpu(mmSRBM_SOFT_RESET, value & ~srbmReset);
	ReadReg4AmdGpu(mmSRBM_SOFT_RESET);
	snooze(100);

	value = ReadReg4AmdGpu(mmGMCON_DEBUG);
	WriteReg4AmdGpu(mmGMCON_DEBUG, value & ~(GMCON_DEBUG__GFX_STALL_MASK
		| GMCON_DEBUG__GFX_CLEAR_MASK));
	snooze(100);
}


static void
WaitForRlcSerdes()
{
	// gfx_v8_0_wait_for_rlc_serdes(), with the broadcast index
	bigtime_t start = system_time();
	while (ReadReg4AmdGpu(mmRLC_SERDES_CU_MASTER_BUSY) != 0
		|| (ReadReg4AmdGpu(mmRLC_SERDES_NONCU_MASTER_BUSY)
			& (RLC_SERDES_NONCU_MASTER_BUSY__SE_MASTER_BUSY_MASK
				| RLC_SERDES_NONCU_MASTER_BUSY__GC_MASTER_BUSY_MASK
				| RLC_SERDES_NONCU_MASTER_BUSY__TC0_MASTER_BUSY_MASK
				| RLC_SERDES_NONCU_MASTER_BUSY__TC1_MASTER_BUSY_MASK)) != 0) {
		if (system_time() - start > 100000) {
			printf("  RLC serdes still busy (%#" B_PRIx32 ", %#" B_PRIx32
				")\n", ReadReg4AmdGpu(mmRLC_SERDES_CU_MASTER_BUSY),
				ReadReg4AmdGpu(mmRLC_SERDES_NONCU_MASTER_BUSY));
			return;
		}
		snooze(10);
	}
}


PolarisGfx::PolarisGfx():
	fRingDwords(0), fWptr(0), fRegistersSaved(false)
{
}


PolarisGfx::~PolarisGfx()
{
	Fini();
}


status_t
PolarisGfx::Init()
{
	if (!gDevice.RegsWritable())
		return B_NOT_ALLOWED;

	auto memMgr = gDevice.MemMgr().Switch();
	fRingDwords = 16384;
	fRing.SetTo(memMgr->Alloc(boDomainVramMappable, fRingDwords * 4, 4096));
	fRptr.SetTo(memMgr->Alloc(boDomainVramMappable, B_PAGE_SIZE));
	if (fRing.adr == NULL || fRptr.adr == NULL)
		return B_NO_MEMORY;
	memset(fRing.adr, 0, fRingDwords * 4);
	memset(fRptr.adr, 0, B_PAGE_SIZE);
	PolarisFlushHdp();

	for (uint32 i = 0; i < B_COUNT_OF(kSavedRegisters); i++)
		sSavedValues[i] = ReadReg4AmdGpu(kSavedRegisters[i]);
	fRegistersSaved = true;

	// gfx_v8_0_enable_gui_idle_interrupt(false), and the EOP interrupt off:
	// a CP interrupt the IH doesn't take (ring disabled) stays pending and
	// keeps the CP busy (CPF_STATUS INTERRUPT_BUSY), also after a reset
	WriteReg4AmdGpu(mmCP_INT_CNTL_RING0, 0);
	HaltCp();
	// SoftReset() isn't used: two runs with it hung the machine right
	// after a fresh firmware load (Linux only resets to recover a hang)

	// *** gfx_v8_0_init_golden_registers()
	for (uint32 i = 0; i < B_COUNT_OF(kGoldenSettings); i += 3) {
		uint32 value;
		if (kGoldenSettings[i + 1] == 0xffffffff)
			value = kGoldenSettings[i + 2];
		else {
			value = ReadReg4AmdGpu(kGoldenSettings[i]);
			value = (value & ~kGoldenSettings[i + 1]) | kGoldenSettings[i + 2];
		}
		WriteReg4AmdGpu(kGoldenSettings[i], value);
	}

	// *** gfx_v8_0_rlc_resume(): stop, reset, start (no power gating)
	uint32 value = ReadReg4AmdGpu(mmRLC_CNTL);
	WriteReg4AmdGpu(mmRLC_CNTL, SET_FIELD(value, RLC_CNTL, RLC_ENABLE_F32, 0));
	WaitForRlcSerdes();
	value = ReadReg4AmdGpu(mmGRBM_SOFT_RESET);
	WriteReg4AmdGpu(mmGRBM_SOFT_RESET,
		SET_FIELD(value, GRBM_SOFT_RESET, SOFT_RESET_RLC, 1));
	snooze(50);
	value = ReadReg4AmdGpu(mmGRBM_SOFT_RESET);
	WriteReg4AmdGpu(mmGRBM_SOFT_RESET,
		SET_FIELD(value, GRBM_SOFT_RESET, SOFT_RESET_RLC, 0));
	snooze(50);
	value = ReadReg4AmdGpu(mmRLC_CNTL);
	WriteReg4AmdGpu(mmRLC_CNTL, SET_FIELD(value, RLC_CNTL, RLC_ENABLE_F32, 1));
	snooze(50);

	// *** gfx_v8_0_cp_gfx_resume()
	WriteReg4AmdGpu(mmCP_RB_WPTR_DELAY, 0);
	WriteReg4AmdGpu(mmCP_RB_VMID, 0);

	uint32 bufSize = OrderBase2(fRingDwords * 4 / 8);
	uint32 rbCntl = SET_FIELD(0, CP_RB0_CNTL, RB_BUFSZ, bufSize);
	rbCntl = SET_FIELD(rbCntl, CP_RB0_CNTL, RB_BLKSZ, bufSize - 2);
	rbCntl = SET_FIELD(rbCntl, CP_RB0_CNTL, MTYPE, 3);
	rbCntl = SET_FIELD(rbCntl, CP_RB0_CNTL, MIN_IB_AVAILSZ, 1);
	WriteReg4AmdGpu(mmCP_RB0_CNTL, rbCntl);

	WriteReg4AmdGpu(mmCP_RB0_CNTL, rbCntl | CP_RB0_CNTL__RB_RPTR_WR_ENA_MASK);
	fWptr = 0;
	WriteReg4AmdGpu(mmCP_RB0_WPTR, 0);

	uint64 rptrAddress = fRptr.buf->gpuPhysAdr;
	WriteReg4AmdGpu(mmCP_RB0_RPTR_ADDR, (uint32)rptrAddress);
	WriteReg4AmdGpu(mmCP_RB0_RPTR_ADDR_HI, (rptrAddress >> 32) & 0xff);
	// write pointer polling isn't used; point it at the same page
	uint64 wptrAddress = rptrAddress + 64;
	WriteReg4AmdGpu(mmCP_RB_WPTR_POLL_ADDR_LO, (uint32)wptrAddress);
	WriteReg4AmdGpu(mmCP_RB_WPTR_POLL_ADDR_HI, wptrAddress >> 32);
	snooze(1000);
	WriteReg4AmdGpu(mmCP_RB0_CNTL, rbCntl);

	uint64 ringAddress = fRing.buf->gpuPhysAdr >> 8;
	WriteReg4AmdGpu(mmCP_RB0_BASE, (uint32)ringAddress);
	WriteReg4AmdGpu(mmCP_RB0_BASE_HI, ringAddress >> 32);

	value = ReadReg4AmdGpu(mmCP_RB_DOORBELL_CONTROL);
	WriteReg4AmdGpu(mmCP_RB_DOORBELL_CONTROL,
		SET_FIELD(value, CP_RB_DOORBELL_CONTROL, DOORBELL_EN, 0));

	// *** gfx_v8_0_cp_gfx_start()
	WriteReg4AmdGpu(mmCP_MAX_CONTEXT, kMaxHwContexts - 1);
	WriteReg4AmdGpu(mmCP_ENDIAN_SWAP, 0);
	WriteReg4AmdGpu(mmCP_DEVICE_ID, 1);

	value = ReadReg4AmdGpu(mmCP_ME_CNTL);
	value = SET_FIELD(value, CP_ME_CNTL, ME_HALT, 0);
	value = SET_FIELD(value, CP_ME_CNTL, PFP_HALT, 0);
	value = SET_FIELD(value, CP_ME_CNTL, CE_HALT, 0);
	WriteReg4AmdGpu(mmCP_ME_CNTL, value);
	snooze(50);

	EmitClearState();
	Commit();

	printf("GFX:       ring %" B_PRIu32 " dwords at %#" B_PRIx64 ", rptr at %#"
		B_PRIx64 ", CP running\n", fRingDwords, fRing.buf->gpuPhysAdr,
		rptrAddress);
	return B_OK;
}


void
PolarisGfx::EmitClearState()
{
	Write(PACKET3(PACKET3_PREAMBLE_CNTL, 0));
	Write(PACKET3_PREAMBLE_BEGIN_CLEAR_STATE);

	Write(PACKET3(PACKET3_CONTEXT_CONTROL, 1));
	Write(0x80000000);
	Write(0x80000000);

	for (const cs_section_def *section = vi_cs_data;
			section->section != NULL; section++) {
		for (const cs_extent_def *extent = section->section;
				extent->extent != NULL; extent++) {
			if (section->id != SECT_CONTEXT)
				continue;
			Write(PACKET3(PACKET3_SET_CONTEXT_REG, extent->reg_count));
			Write(extent->reg_index - PACKET3_SET_CONTEXT_REG_START);
			for (uint32 i = 0; i < extent->reg_count; i++)
				Write(extent->extent[i]);
		}
	}

	Write(PACKET3(PACKET3_SET_CONTEXT_REG, 2));
	Write(mmPA_SC_RASTER_CONFIG - PACKET3_SET_CONTEXT_REG_START);
	Write(kRasterConfig);
	Write(kRasterConfig1);

	Write(PACKET3(PACKET3_PREAMBLE_CNTL, 0));
	Write(PACKET3_PREAMBLE_END_CLEAR_STATE);

	Write(PACKET3(PACKET3_CLEAR_STATE, 0));
	Write(0);

	// init the CE partitions
	Write(PACKET3(PACKET3_SET_BASE, 2));
	Write(PACKET3_BASE_INDEX(CE_PARTITION_BASE));
	Write(0x8000);
	Write(0x8000);
}


void
PolarisGfx::Fini()
{
	if (!fRegistersSaved)
		return;

	// no CP interrupts once the IH is gone (the VBIOS value 0x003c0000
	// enables the GUI busy/idle interrupts)
	WriteReg4AmdGpu(mmCP_INT_CNTL_RING0, 0);
	HaltCp();
	uint32 value = ReadReg4AmdGpu(mmRLC_CNTL);
	WriteReg4AmdGpu(mmRLC_CNTL, SET_FIELD(value, RLC_CNTL, RLC_ENABLE_F32, 0));
	WaitForRlcSerdes();

	for (int32 i = B_COUNT_OF(kSavedRegisters) - 1; i >= 0; i--)
		WriteReg4AmdGpu(kSavedRegisters[i], sSavedValues[i]);
	fRegistersSaved = false;
	printf("GFX:       CP and RLC halted, registers restored\n");
}


status_t
PolarisGfx::Begin(uint32 dwords)
{
	if (dwords + 256 >= fRingDwords)
		return B_BAD_VALUE;
	return B_OK;
}


void
PolarisGfx::Write(uint32 dword)
{
	((volatile uint32*)fRing.adr)[fWptr] = dword;
	fWptr = (fWptr + 1) % fRingDwords;
}


void
PolarisGfx::Commit()
{
	// amdgpu_ring_commit(): pad to the ring alignment (256 dwords) with
	// the gfx ring's one dword NOP
	while ((fWptr & 0xff) != 0)
		Write(PACKET3(PACKET3_NOP, 0x3fff));

	__sync_synchronize();
	PolarisFlushHdp();
	WriteReg4AmdGpu(mmCP_RB0_WPTR, fWptr);
	ReadReg4AmdGpu(mmCP_RB0_WPTR);
}


status_t
PolarisGfx::WaitIdle(bigtime_t timeout)
{
	bigtime_t start = system_time();
	for (;;) {
		uint32 rptr = ReadReg4AmdGpu(mmCP_RB0_RPTR);
		uint32 status = ReadReg4AmdGpu(mmGRBM_STATUS);
		if (rptr == fWptr && (status & GRBM_STATUS__GUI_ACTIVE_MASK) == 0)
			return B_OK;
		if (system_time() - start > timeout) {
			if (rptr == fWptr) {
				// everything fetched; report the busy state, but go on
				printf("  (ring consumed, GRBM_STATUS %#010" B_PRIx32
					" still busy)\n", status);
				return B_OK;
			}
			return B_TIMED_OUT;
		}
		snooze(10);
	}
}


void
PolarisGfx::PrintState()
{
	printf("  GRBM_STATUS %#010" B_PRIx32 ", GRBM_STATUS2 %#010" B_PRIx32
		", CP_STAT %#010" B_PRIx32 ", CP_BUSY_STAT %#010" B_PRIx32 "\n",
		ReadReg4AmdGpu(mmGRBM_STATUS), ReadReg4AmdGpu(mmGRBM_STATUS2),
		ReadReg4AmdGpu(mmCP_STAT), ReadReg4AmdGpu(mmCP_BUSY_STAT));
	printf("  CP_STALLED_STAT1/2/3 %#" B_PRIx32 " %#" B_PRIx32 " %#" B_PRIx32
		", ME/PFP header %#010" B_PRIx32 " %#010" B_PRIx32 "\n",
		ReadReg4AmdGpu(mmCP_STALLED_STAT1), ReadReg4AmdGpu(mmCP_STALLED_STAT2),
		ReadReg4AmdGpu(mmCP_STALLED_STAT3), ReadReg4AmdGpu(mmCP_ME_HEADER_DUMP),
		ReadReg4AmdGpu(mmCP_PFP_HEADER_DUMP));
	printf("  CP_CPF_STATUS %#010" B_PRIx32 ", CP_INT_CNTL_RING0 %#010" B_PRIx32
		"\n", ReadReg4AmdGpu(mmCP_CPF_STATUS),
		ReadReg4AmdGpu(mmCP_INT_CNTL_RING0));
	printf("  CP_ME_CNTL %#010" B_PRIx32 ", CP_RB0_RPTR %#" B_PRIx32
		", our wptr %#" B_PRIx32 ", RLC_CNTL %#" B_PRIx32 ", RLC_STAT %#"
		B_PRIx32 ", RLC_GPM_STAT %#" B_PRIx32 "\n",
		ReadReg4AmdGpu(mmCP_ME_CNTL), ReadReg4AmdGpu(mmCP_RB0_RPTR), fWptr,
		ReadReg4AmdGpu(mmRLC_CNTL), ReadReg4AmdGpu(mmRLC_STAT),
		ReadReg4AmdGpu(mmRLC_GPM_STAT));
}


void
PolarisGfx::EnableEopInterrupt(bool enable)
{
	uint32 value = ReadReg4AmdGpu(mmCP_INT_CNTL_RING0);
	WriteReg4AmdGpu(mmCP_INT_CNTL_RING0, SET_FIELD(value, CP_INT_CNTL_RING0,
		TIME_STAMP_INT_ENABLE, enable ? 1 : 0));
}


void
PolarisGfx::EmitSetUconfigReg(uint32 reg, uint32 value)
{
	Write(PACKET3(PACKET3_SET_UCONFIG_REG, 1));
	Write(reg - PACKET3_SET_UCONFIG_REG_START);
	Write(value);
}


void
PolarisGfx::EmitWriteData(uint64 address, uint32 value)
{
	// destination: memory, with write confirmation
	Write(PACKET3(PACKET3_WRITE_DATA, 3));
	Write(WRITE_DATA_DST_SEL(5) | WR_CONFIRM | WRITE_DATA_ENGINE_SEL(0));
	Write((uint32)address & 0xfffffffc);
	Write((uint32)(address >> 32));
	Write(value);
}


void
PolarisGfx::EmitFence(uint64 address, uint32 value, bool interrupt)
{
	// gfx_v8_0_ring_emit_fence_gfx(), 32 bit
	Write(PACKET3(PACKET3_EVENT_WRITE_EOP, 4));
	Write(EOP_TCL1_ACTION_EN | EOP_TC_ACTION_EN | EOP_TC_WB_ACTION_EN
		| EVENT_TYPE(CACHE_FLUSH_AND_INV_TS_EVENT) | EVENT_INDEX(5));
	Write((uint32)address & 0xfffffffc);
	Write(((uint32)(address >> 32) & 0xffff) | DATA_SEL(1)
		| INT_SEL(interrupt ? 2 : 0));
	Write(value);
	Write(0);
}


void
PolarisGfx::EmitSetComputeReg(uint32 reg, const uint32 *values, uint32 count)
{
	Write(PACKET3_COMPUTE(PACKET3_SET_SH_REG, count));
	Write(reg - PACKET3_SET_SH_REG_START);
	for (uint32 i = 0; i < count; i++)
		Write(values[i]);
}


void
PolarisGfx::EmitDispatch(uint32 x, uint32 y, uint32 z)
{
	Write(PACKET3_COMPUTE(PACKET3_DISPATCH_DIRECT, 3));
	Write(x);
	Write(y);
	Write(z);
	Write(COMPUTE_DISPATCH_INITIATOR__COMPUTE_SHADER_EN_MASK);
}


void
PolarisGfx::EmitCsPartialFlush()
{
	Write(PACKET3(PACKET3_EVENT_WRITE, 0));
	Write(EVENT_TYPE(CS_PARTIAL_FLUSH) | EVENT_INDEX(4));
}


void
PolarisGfx::SetupShaderMemory()
{
	// *** gfx_v8_0_constants_init(): shader memory for VMID 0
	uint32 value = SET_FIELD(0, SH_STATIC_MEM_CONFIG, SWIZZLE_ENABLE, 1);
	value = SET_FIELD(value, SH_STATIC_MEM_CONFIG, ELEMENT_SIZE, 1);
	value = SET_FIELD(value, SH_STATIC_MEM_CONFIG, INDEX_STRIDE, 3);
	WriteReg4AmdGpu(mmSH_STATIC_MEM_CONFIG, value);
	WriteReg4AmdGpu(mmSRBM_GFX_CNTL, 0);
	value = SET_FIELD(0, SH_MEM_CONFIG, DEFAULT_MTYPE, kMtypeUc);
	value = SET_FIELD(value, SH_MEM_CONFIG, APE1_MTYPE, kMtypeUc);
	value = SET_FIELD(value, SH_MEM_CONFIG, ALIGNMENT_MODE,
		kShMemAlignmentModeUnaligned);
	WriteReg4AmdGpu(mmSH_MEM_CONFIG, value);
	WriteReg4AmdGpu(mmSH_MEM_BASES, 0);
	WriteReg4AmdGpu(mmSH_MEM_APE1_BASE, 1);
	WriteReg4AmdGpu(mmSH_MEM_APE1_LIMIT, 0);

}
