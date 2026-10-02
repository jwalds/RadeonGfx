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
#include "PolarisGfxPackets.h"

// gfx_8_0_enum.h
#define CACHE_FLUSH_AND_INV_TS_EVENT	0x14
#define CS_PARTIAL_FLUSH				0x7
static const uint32 kMtypeNc = 1;
static const uint32 kMtypeUc = 3;
// gmc_v8_0.c: shared aperture of the per process address spaces
static const uint64 kSharedApertureBase = 0x2000000000000000ULL;
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
static const uint32 kScPrimFifoSizeFrontend = 0x20;
static const uint32 kScPrimFifoSizeBackend = 0x100;
static const uint32 kScHizTileFifoSize = 0x30;
static const uint32 kScEarlyzTileFifoSize = 0x130;

// gfx_v8_0_tiling_mode_table_init(), Polaris 11 (computed from Linux' table)
static const uint32 kTileModes[32] = {
	0x00800150, 0x00800950, 0x00801150, 0x00801950,
	0x00802950, 0x00802948, 0x00802954, 0x00802954,
	0x00000144, 0x02000148, 0x02000150, 0x06000154,
	0x06000154, 0x02400148, 0x02400150, 0x02400170,
	0x06400154, 0x06400154, 0x0040014c, 0x0100014c,
	0x0100015c, 0x01000174, 0x01000164, 0x01000164,
	0x0040015c, 0x01000160, 0x01000178, 0x02c00148,
	0x02c00150, 0x06c00154, 0x06c00154, 0x00000000,
};
static const uint32 kMacroTileModes[16] = {
	0x000000e8, 0x000000e8, 0x000000e8, 0x000000e4,
	0x000000d0, 0x000000d0, 0x000000d0, 0x00000000,
	0x000000ed, 0x000000e9, 0x000000e8, 0x000000e4,
	0x000000d0, 0x00000090, 0x00000040, 0x00000000,
};

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
	// constants
	mmPA_SC_FIFO_SIZE, mmSPI_ARB_PRIORITY,
	mmGB_TILE_MODE0, mmGB_TILE_MODE1, mmGB_TILE_MODE2, mmGB_TILE_MODE3,
	mmGB_TILE_MODE4, mmGB_TILE_MODE5, mmGB_TILE_MODE6, mmGB_TILE_MODE7,
	mmGB_TILE_MODE8, mmGB_TILE_MODE9, mmGB_TILE_MODE10, mmGB_TILE_MODE11,
	mmGB_TILE_MODE12, mmGB_TILE_MODE13, mmGB_TILE_MODE14, mmGB_TILE_MODE15,
	mmGB_TILE_MODE16, mmGB_TILE_MODE17, mmGB_TILE_MODE18, mmGB_TILE_MODE19,
	mmGB_TILE_MODE20, mmGB_TILE_MODE21, mmGB_TILE_MODE22, mmGB_TILE_MODE23,
	mmGB_TILE_MODE24, mmGB_TILE_MODE25, mmGB_TILE_MODE26, mmGB_TILE_MODE27,
	mmGB_TILE_MODE28, mmGB_TILE_MODE29, mmGB_TILE_MODE30, mmGB_TILE_MODE31,
	mmGB_MACROTILE_MODE0, mmGB_MACROTILE_MODE1, mmGB_MACROTILE_MODE2,
	mmGB_MACROTILE_MODE3, mmGB_MACROTILE_MODE4, mmGB_MACROTILE_MODE5,
	mmGB_MACROTILE_MODE6, mmGB_MACROTILE_MODE8, mmGB_MACROTILE_MODE9,
	mmGB_MACROTILE_MODE10, mmGB_MACROTILE_MODE11, mmGB_MACROTILE_MODE12,
	mmGB_MACROTILE_MODE13, mmGB_MACROTILE_MODE14, mmGB_MACROTILE_MODE15,
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

	{
		auto memMgr = gDevice.MemMgr().Switch();
		fRingDwords = 16384;
		fRing.SetTo(memMgr->Alloc(boDomainVramMappable, fRingDwords * 4,
			4096));
		fRptr.SetTo(memMgr->Alloc(boDomainVramMappable, B_PAGE_SIZE));
		if (fRing.adr == NULL || fRptr.adr == NULL)
			return B_NO_MEMORY;
		memset(fRing.adr, 0, fRingDwords * 4);
		memset(fRptr.adr, 0, B_PAGE_SIZE);
	}
	PolarisFlushHdp();

	CheckRet(InitHardware(fRing.buf->gpuPhysAdr, fRingDwords,
		fRptr.buf->gpuPhysAdr));
	fWptr = 0;
	EmitClearState();
	Commit();

	printf("GFX:       ring %" B_PRIu32 " dwords at %#" B_PRIx64 ", rptr at %#"
		B_PRIx64 ", CP running\n", fRingDwords, fRing.buf->gpuPhysAdr,
		fRptr.buf->gpuPhysAdr);
	return B_OK;
}


status_t
PolarisGfx::InitHardware(uint64 ringAddress, uint32 ringDwords,
	uint64 rptrAddress)
{
	if (!gDevice.RegsWritable())
		return B_NOT_ALLOWED;

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

	// *** gfx_v8_0_constants_init(), the parts Mesa depends on
	InitTiling();
	WriteReg4AmdGpu(mmPA_SC_FIFO_SIZE,
		(kScPrimFifoSizeFrontend
			<< PA_SC_FIFO_SIZE__SC_FRONTEND_PRIM_FIFO_SIZE__SHIFT)
		| (kScPrimFifoSizeBackend
			<< PA_SC_FIFO_SIZE__SC_BACKEND_PRIM_FIFO_SIZE__SHIFT)
		| (kScHizTileFifoSize << PA_SC_FIFO_SIZE__SC_HIZ_TILE_FIFO_SIZE__SHIFT)
		| (kScEarlyzTileFifoSize
			<< PA_SC_FIFO_SIZE__SC_EARLYZ_TILE_FIFO_SIZE__SHIFT));
	uint32 value = ReadReg4AmdGpu(mmSPI_ARB_PRIORITY);
	value = SET_FIELD(value, SPI_ARB_PRIORITY, PIPE_ORDER_TS0, 2);
	value = SET_FIELD(value, SPI_ARB_PRIORITY, PIPE_ORDER_TS1, 2);
	value = SET_FIELD(value, SPI_ARB_PRIORITY, PIPE_ORDER_TS2, 2);
	value = SET_FIELD(value, SPI_ARB_PRIORITY, PIPE_ORDER_TS3, 2);
	WriteReg4AmdGpu(mmSPI_ARB_PRIORITY, value);
	SetupShaderMemory();

	// *** gfx_v8_0_rlc_resume(): stop, reset, start (no power gating)
	value = ReadReg4AmdGpu(mmRLC_CNTL);
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

	uint32 bufSize = OrderBase2(ringDwords * 4 / 8);
	uint32 rbCntl = SET_FIELD(0, CP_RB0_CNTL, RB_BUFSZ, bufSize);
	rbCntl = SET_FIELD(rbCntl, CP_RB0_CNTL, RB_BLKSZ, bufSize - 2);
	// as in the working bring-up test (ring in VRAM); Linux leaves both 0,
	// but its rings are in system memory
	rbCntl = SET_FIELD(rbCntl, CP_RB0_CNTL, MTYPE, 3);
	rbCntl = SET_FIELD(rbCntl, CP_RB0_CNTL, MIN_IB_AVAILSZ, 1);
	WriteReg4AmdGpu(mmCP_RB0_CNTL, rbCntl);

	WriteReg4AmdGpu(mmCP_RB0_CNTL, rbCntl | CP_RB0_CNTL__RB_RPTR_WR_ENA_MASK);
	WriteReg4AmdGpu(mmCP_RB0_WPTR, 0);

	WriteReg4AmdGpu(mmCP_RB0_RPTR_ADDR, (uint32)rptrAddress);
	WriteReg4AmdGpu(mmCP_RB0_RPTR_ADDR_HI, (rptrAddress >> 32) & 0xff);
	// write pointer polling isn't used; point it at the same page
	uint64 wptrAddress = rptrAddress + 64;
	WriteReg4AmdGpu(mmCP_RB_WPTR_POLL_ADDR_LO, (uint32)wptrAddress);
	WriteReg4AmdGpu(mmCP_RB_WPTR_POLL_ADDR_HI, wptrAddress >> 32);
	snooze(1000);
	WriteReg4AmdGpu(mmCP_RB0_CNTL, rbCntl);

	WriteReg4AmdGpu(mmCP_RB0_BASE, (uint32)(ringAddress >> 8));
	WriteReg4AmdGpu(mmCP_RB0_BASE_HI, ringAddress >> 40);

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
	return B_OK;
}


void
PolarisGfx::InitTiling()
{
	// gfx_v8_0_tiling_mode_table_init(), Polaris 11
	for (uint32 i = 0; i < B_COUNT_OF(kTileModes); i++)
		WriteReg4AmdGpu(mmGB_TILE_MODE0 + i, kTileModes[i]);
	for (uint32 i = 0; i < B_COUNT_OF(kMacroTileModes); i++) {
		if (i != 7)
			WriteReg4AmdGpu(mmGB_MACROTILE_MODE0 + i, kMacroTileModes[i]);
	}
}


void
PolarisGfx::EmitClearState()
{
	GenClearState(*this);
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
PolarisGfx::PrintVmFaults()
{
	printf("  VM context 0: fault status %#010" B_PRIx32 ", address %#010"
		B_PRIx32 ", client %#010" B_PRIx32 ", CNTL %#010" B_PRIx32 "\n",
		ReadReg4AmdGpu(mmVM_CONTEXT0_PROTECTION_FAULT_STATUS),
		ReadReg4AmdGpu(mmVM_CONTEXT0_PROTECTION_FAULT_ADDR),
		ReadReg4AmdGpu(mmVM_CONTEXT0_PROTECTION_FAULT_MCCLIENT),
		ReadReg4AmdGpu(mmVM_CONTEXT0_CNTL));
	printf("  VM context 1: fault status %#010" B_PRIx32 ", address %#010"
		B_PRIx32 ", client %#010" B_PRIx32 ", CNTL %#010" B_PRIx32 "\n",
		ReadReg4AmdGpu(mmVM_CONTEXT1_PROTECTION_FAULT_STATUS),
		ReadReg4AmdGpu(mmVM_CONTEXT1_PROTECTION_FAULT_ADDR),
		ReadReg4AmdGpu(mmVM_CONTEXT1_PROTECTION_FAULT_MCCLIENT),
		ReadReg4AmdGpu(mmVM_CONTEXT1_CNTL));
	printf("  CP_RB_VMID %#" B_PRIx32 ", VM_L2_STATUS %#" B_PRIx32 "\n",
		ReadReg4AmdGpu(mmCP_RB_VMID), ReadReg4AmdGpu(mmVM_L2_STATUS));
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
PolarisGfx::EmitWriteData(uint64 address, uint32 value, bool throughL2)
{
	// destination: memory (5) or TC L2 (2), with write confirmation
	Write(PACKET3(PACKET3_WRITE_DATA, 3));
	Write(WRITE_DATA_DST_SEL(throughL2 ? 2 : 5) | WR_CONFIRM
		| WRITE_DATA_ENGINE_SEL(0));
	Write((uint32)address & 0xfffffffc);
	Write((uint32)(address >> 32));
	Write(value);
}


void
PolarisGfx::EmitCopyData(uint64 source, uint64 destination, bool throughL2)
{
	// one dword, source memory (1) or TC L2 (2), destination memory (5)
	Write(PACKET3(PACKET3_COPY_DATA, 4));
	Write((throughL2 ? 2 : 1) | (5 << 8) | WR_CONFIRM);
	Write((uint32)source);
	Write((uint32)(source >> 32));
	Write((uint32)destination);
	Write((uint32)(destination >> 32));
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
	for (uint32 vmid = 0; vmid < 16; vmid++) {
		// vi_srbm_select(): the SH_MEM registers are per VMID
		WriteReg4AmdGpu(mmSRBM_GFX_CNTL, vmid << SRBM_GFX_CNTL__VMID__SHIFT);
		if (vmid == 0) {
			value = SET_FIELD(0, SH_MEM_CONFIG, DEFAULT_MTYPE, kMtypeUc);
			WriteReg4AmdGpu(mmSH_MEM_BASES, 0);
		} else {
			value = SET_FIELD(0, SH_MEM_CONFIG, DEFAULT_MTYPE, kMtypeNc);
			WriteReg4AmdGpu(mmSH_MEM_BASES, kSharedApertureBase >> 48);
		}
		value = SET_FIELD(value, SH_MEM_CONFIG, APE1_MTYPE, kMtypeUc);
		value = SET_FIELD(value, SH_MEM_CONFIG, ALIGNMENT_MODE,
			kShMemAlignmentModeUnaligned);
		WriteReg4AmdGpu(mmSH_MEM_CONFIG, value);
		WriteReg4AmdGpu(mmSH_MEM_APE1_BASE, 1);
		WriteReg4AmdGpu(mmSH_MEM_APE1_LIMIT, 0);
	}
	WriteReg4AmdGpu(mmSRBM_GFX_CNTL, 0);

}


static uint32
ReadWave(uint32 simd, uint32 wave, uint32 index)
{
	WriteReg4AmdGpu(mmSQ_IND_INDEX, (index << SQ_IND_INDEX__INDEX__SHIFT)
		| (simd << SQ_IND_INDEX__SIMD_ID__SHIFT)
		| (wave << SQ_IND_INDEX__WAVE_ID__SHIFT)
		| SQ_IND_INDEX__FORCE_READ_MASK);
	return ReadReg4AmdGpu(mmSQ_IND_DATA);
}


void
PolarisGfx::DumpWaves(uint32 maxWaves)
{
	uint32 count = 0;
	for (uint32 se = 0; se < 2; se++) {
		for (uint32 cu = 0; cu < 8; cu++) {
			WriteReg4AmdGpu(mmGRBM_GFX_INDEX,
				(se << GRBM_GFX_INDEX__SE_INDEX__SHIFT)
				| (cu << GRBM_GFX_INDEX__INSTANCE_INDEX__SHIFT)
				| GRBM_GFX_INDEX__SH_BROADCAST_WRITES_MASK);
			for (uint32 simd = 0; simd < 4; simd++) {
				for (uint32 wave = 0; wave < 10; wave++) {
					uint32 status = ReadWave(simd, wave, ixSQ_WAVE_STATUS);
					if ((status & SQ_WAVE_STATUS__VALID_MASK) == 0)
						continue;
					if (count++ >= maxWaves)
						continue;
					printf("  wave SE%" B_PRIu32 " CU%" B_PRIu32 " SIMD%" B_PRIu32
						" W%" B_PRIu32 ": status %#010" B_PRIx32 ", pc %#"
						B_PRIx64 ", trapsts %#010" B_PRIx32 ", ib_sts %#010"
						B_PRIx32 ", inst %#010" B_PRIx32 "\n", se, cu, simd,
						wave, status,
						((uint64)ReadWave(simd, wave, ixSQ_WAVE_PC_HI) << 32)
							| ReadWave(simd, wave, ixSQ_WAVE_PC_LO),
						ReadWave(simd, wave, ixSQ_WAVE_TRAPSTS),
						ReadWave(simd, wave, ixSQ_WAVE_IB_STS),
						ReadWave(simd, wave, ixSQ_WAVE_INST_DW0));
				}
			}
		}
	}
	WriteReg4AmdGpu(mmGRBM_GFX_INDEX, GRBM_GFX_INDEX__SE_BROADCAST_WRITES_MASK
		| GRBM_GFX_INDEX__SH_BROADCAST_WRITES_MASK
		| GRBM_GFX_INDEX__INSTANCE_BROADCAST_WRITES_MASK);
	printf("  %" B_PRIu32 " valid wave(s)\n", count);
}
