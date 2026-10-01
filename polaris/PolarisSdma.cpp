#include "PolarisSdma.h"
#include "RadeonDevice.h"
#include "Radeon.h"

#include <stdio.h>
#include <string.h>
#include <File.h>
#include <OS.h>

#include "vi/oss_3_0_d.h"
#include "vi/oss_3_0_sh_mask.h"
#include "vi/bif_5_0_d.h"
#include "vi/gfx_8_0_d.h"
#include "vi/tonga_sdma_pkt_open.h"

#define CheckRet(err) {status_t _err = (err); if (_err < B_OK) return _err;}

#define SET_FIELD(value, reg, field, fieldValue) \
	(((value) & ~reg##__##field##_MASK) \
		| (((uint32)(fieldValue) << reg##__##field##__SHIFT) \
			& reg##__##field##_MASK))


// amdgpu firmware headers (amdgpu_ucode.h)
struct common_firmware_header {
	uint32 size_bytes;
	uint32 header_size_bytes;
	uint16 header_version_major;
	uint16 header_version_minor;
	uint16 ip_version_major;
	uint16 ip_version_minor;
	uint32 ucode_version;
	uint32 ucode_size_bytes;
	uint32 ucode_array_offset_bytes;
	uint32 crc32;
};

struct sdma_firmware_header_v1_0 {
	common_firmware_header header;
	uint32 ucode_feature_version;
	uint32 ucode_change_version;
	uint32 jt_offset;
	uint32 jt_size;
};


// Linux golden_settings_polaris11_a11, SDMA0 part: register, mask, value
static const uint32 kGoldenSettings[] = {
	mmSDMA0_CHICKEN_BITS, 0xfc910007, 0x00810007,
	mmSDMA0_CLK_CTRL, 0xff000fff, 0x00000000,
	mmSDMA0_GFX_IB_CNTL, 0x800f0111, 0x00000100,
	mmSDMA0_RLC0_IB_CNTL, 0x800f0111, 0x00000100,
	mmSDMA0_RLC1_IB_CNTL, 0x800f0111, 0x00000100,
};

static const uint32 kSavedRegisters[] = {
	mmSDMA0_CHICKEN_BITS,
	mmSDMA0_CLK_CTRL,
	mmSDMA0_RLC0_IB_CNTL,
	mmSDMA0_RLC1_IB_CNTL,
	mmSDMA0_TILING_CONFIG,
	mmSDMA0_SEM_WAIT_FAIL_TIMER_CNTL,
	mmSDMA0_GFX_RB_BASE,
	mmSDMA0_GFX_RB_BASE_HI,
	mmSDMA0_GFX_RB_RPTR_ADDR_HI,
	mmSDMA0_GFX_RB_RPTR_ADDR_LO,
	mmSDMA0_GFX_DOORBELL,
	mmSDMA0_GFX_RB_WPTR_POLL_ADDR_LO,
	mmSDMA0_GFX_RB_WPTR_POLL_ADDR_HI,
	mmSDMA0_GFX_RB_WPTR_POLL_CNTL,
	mmSDMA0_GFX_IB_CNTL,
	mmSDMA0_GFX_RB_CNTL,
	mmSDMA0_CNTL,
	mmSDMA0_F32_CNTL,
};

static uint32 sSavedValues[B_COUNT_OF(kSavedRegisters)];


void
PolarisFlushHdp()
{
	// amdgpu_asic_flush_hdp() on VI
	WriteReg4AmdGpu(mmHDP_MEM_COHERENCY_FLUSH_CNTL, 1);
	ReadReg4AmdGpu(mmHDP_MEM_COHERENCY_FLUSH_CNTL);
}


void
PolarisInvalidateHdp()
{
	// amdgpu_asic_invalidate_hdp() on VI
	WriteReg4AmdGpu(mmHDP_DEBUG0, 1);
	ReadReg4AmdGpu(mmHDP_DEBUG0);
}


static uint32
OrderBase2(uint32 value)
{
	uint32 order = 0;
	while ((1u << order) < value)
		order++;
	return order;
}


static void
Halt()
{
	// sdma_v3_0_enable(false): stop ring and IBs, halt the F32
	uint32 value = ReadReg4AmdGpu(mmSDMA0_GFX_RB_CNTL);
	WriteReg4AmdGpu(mmSDMA0_GFX_RB_CNTL,
		SET_FIELD(value, SDMA0_GFX_RB_CNTL, RB_ENABLE, 0));
	value = ReadReg4AmdGpu(mmSDMA0_GFX_IB_CNTL);
	WriteReg4AmdGpu(mmSDMA0_GFX_IB_CNTL,
		SET_FIELD(value, SDMA0_GFX_IB_CNTL, IB_ENABLE, 0));
	value = ReadReg4AmdGpu(mmSDMA0_F32_CNTL);
	WriteReg4AmdGpu(mmSDMA0_F32_CNTL, SET_FIELD(value, SDMA0_F32_CNTL, HALT, 1));
}


PolarisSdma::PolarisSdma():
	fRingDwords(0), fWptr(0), fFirmwareVersion(0), fRegistersSaved(false),
	fRunning(false)
{
}


PolarisSdma::~PolarisSdma()
{
	Fini();
}


status_t
PolarisSdma::Init(const char *firmwarePath)
{
	if (!gDevice.RegsWritable())
		return B_NOT_ALLOWED;

	// *** firmware file
	BFile file(firmwarePath, B_READ_ONLY);
	CheckRet(file.InitCheck());
	off_t fileSize;
	CheckRet(file.GetSize(&fileSize));
	if (fileSize < (off_t)sizeof(sdma_firmware_header_v1_0)
		|| fileSize > 1024 * 1024)
		return B_BAD_DATA;
	ArrayDeleter<uint8> data(new(std::nothrow) uint8[fileSize]);
	if (!data.IsSet())
		return B_NO_MEMORY;
	if (file.ReadAt(0, data.Get(), fileSize) != fileSize)
		return B_IO_ERROR;
	const sdma_firmware_header_v1_0 &header
		= *(const sdma_firmware_header_v1_0*)data.Get();
	if (header.header.size_bytes != fileSize
		|| header.header.ucode_array_offset_bytes
			+ header.header.ucode_size_bytes > fileSize
		|| header.header.ip_version_major != 3
		|| header.header.ucode_size_bytes % 4 != 0) {
		printf("[!] unexpected SDMA firmware header\n");
		return B_BAD_DATA;
	}
	fFirmwareVersion = header.header.ucode_version;
	const uint32 *ucode = (const uint32*)(data.Get()
		+ header.header.ucode_array_offset_bytes);
	uint32 ucodeDwords = header.header.ucode_size_bytes / 4;

	// *** ring and read pointer write-back in VRAM
	auto memMgr = gDevice.MemMgr().Switch();
	fRingDwords = 1024;
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

	// *** sdma_v3_0_init_golden_registers()
	for (uint32 i = 0; i < B_COUNT_OF(kGoldenSettings); i += 3) {
		uint32 value = ReadReg4AmdGpu(kGoldenSettings[i]);
		value = (value & ~kGoldenSettings[i + 1]) | kGoldenSettings[i + 2];
		WriteReg4AmdGpu(kGoldenSettings[i], value);
	}

	// *** sdma_v3_0_start(): context switching off, engine halted
	uint32 value = ReadReg4AmdGpu(mmSDMA0_CNTL);
	value = SET_FIELD(value, SDMA0_CNTL, AUTO_CTXSW_ENABLE, 0);
	value = SET_FIELD(value, SDMA0_CNTL, ATC_L1_ENABLE, 1);
	WriteReg4AmdGpu(mmSDMA0_CNTL, value);
	Halt();

	// *** sdma_v3_0_load_microcode() (Linux 4.7, direct loading)
	WriteReg4AmdGpu(mmSDMA0_UCODE_ADDR, 0);
	for (uint32 i = 0; i < ucodeDwords; i++)
		WriteReg4AmdGpu(mmSDMA0_UCODE_DATA, ucode[i]);
	WriteReg4AmdGpu(mmSDMA0_UCODE_ADDR, fFirmwareVersion);
	printf("SDMA0:     firmware %#" B_PRIx32 " (%" B_PRIu32 " dwords) loaded\n",
		fFirmwareVersion, ucodeDwords);

	// *** sdma_v3_0_gfx_resume(), engine 0
	for (uint32 vmid = 0; vmid < 16; vmid++) {
		WriteReg4AmdGpu(mmSRBM_GFX_CNTL,
			SET_FIELD(0, SRBM_GFX_CNTL, VMID, vmid));
		WriteReg4AmdGpu(mmSDMA0_GFX_VIRTUAL_ADDR, 0);
		WriteReg4AmdGpu(mmSDMA0_GFX_APE1_CNTL, 0);
	}
	WriteReg4AmdGpu(mmSRBM_GFX_CNTL, 0);

	WriteReg4AmdGpu(mmSDMA0_TILING_CONFIG,
		ReadReg4AmdGpu(mmGB_ADDR_CONFIG) & 0x70);
	WriteReg4AmdGpu(mmSDMA0_SEM_WAIT_FAIL_TIMER_CNTL, 0);

	uint32 rbCntl = ReadReg4AmdGpu(mmSDMA0_GFX_RB_CNTL);
	rbCntl = SET_FIELD(rbCntl, SDMA0_GFX_RB_CNTL, RB_SIZE,
		OrderBase2(fRingDwords));
	WriteReg4AmdGpu(mmSDMA0_GFX_RB_CNTL, rbCntl);

	fWptr = 0;
	WriteReg4AmdGpu(mmSDMA0_GFX_RB_RPTR, 0);
	WriteReg4AmdGpu(mmSDMA0_GFX_RB_WPTR, 0);
	WriteReg4AmdGpu(mmSDMA0_GFX_IB_RPTR, 0);
	WriteReg4AmdGpu(mmSDMA0_GFX_IB_OFFSET, 0);

	uint64 rptrAddress = fRptr.buf->gpuPhysAdr;
	WriteReg4AmdGpu(mmSDMA0_GFX_RB_RPTR_ADDR_HI, rptrAddress >> 32);
	WriteReg4AmdGpu(mmSDMA0_GFX_RB_RPTR_ADDR_LO,
		(uint32)rptrAddress & 0xfffffffc);
	rbCntl = SET_FIELD(rbCntl, SDMA0_GFX_RB_CNTL, RPTR_WRITEBACK_ENABLE, 1);

	uint64 ringAddress = fRing.buf->gpuPhysAdr;
	WriteReg4AmdGpu(mmSDMA0_GFX_RB_BASE, ringAddress >> 8);
	WriteReg4AmdGpu(mmSDMA0_GFX_RB_BASE_HI, ringAddress >> 40);

	value = ReadReg4AmdGpu(mmSDMA0_GFX_DOORBELL);
	WriteReg4AmdGpu(mmSDMA0_GFX_DOORBELL,
		SET_FIELD(value, SDMA0_GFX_DOORBELL, ENABLE, 0));

	value = ReadReg4AmdGpu(mmSDMA0_GFX_RB_WPTR_POLL_CNTL);
	WriteReg4AmdGpu(mmSDMA0_GFX_RB_WPTR_POLL_CNTL,
		SET_FIELD(value, SDMA0_GFX_RB_WPTR_POLL_CNTL, ENABLE, 0));

	rbCntl = SET_FIELD(rbCntl, SDMA0_GFX_RB_CNTL, RB_ENABLE, 1);
	WriteReg4AmdGpu(mmSDMA0_GFX_RB_CNTL, rbCntl);
	value = ReadReg4AmdGpu(mmSDMA0_GFX_IB_CNTL);
	WriteReg4AmdGpu(mmSDMA0_GFX_IB_CNTL,
		SET_FIELD(value, SDMA0_GFX_IB_CNTL, IB_ENABLE, 1));

	// unhalt; context switching stays off (single ring)
	value = ReadReg4AmdGpu(mmSDMA0_F32_CNTL);
	WriteReg4AmdGpu(mmSDMA0_F32_CNTL, SET_FIELD(value, SDMA0_F32_CNTL, HALT, 0));
	fRunning = true;

	printf("SDMA0:     ring %" B_PRIu32 " dwords at %#" B_PRIx64
		", rptr at %#" B_PRIx64 ", running\n", fRingDwords, ringAddress,
		rptrAddress);
	return B_OK;
}


void
PolarisSdma::Fini()
{
	if (!fRegistersSaved)
		return;

	Halt();
	for (int32 i = B_COUNT_OF(kSavedRegisters) - 1; i >= 0; i--)
		WriteReg4AmdGpu(kSavedRegisters[i], sSavedValues[i]);
	fRegistersSaved = false;
	fRunning = false;
	printf("SDMA0:     halted, registers restored\n");
}


status_t
PolarisSdma::Begin(uint32 dwords)
{
	// the ring is never more than a few packets full in these tests
	if (dwords + 16 >= fRingDwords)
		return B_BAD_VALUE;
	return B_OK;
}


void
PolarisSdma::Write(uint32 dword)
{
	((volatile uint32*)fRing.adr)[fWptr] = dword;
	fWptr = (fWptr + 1) % fRingDwords;
}


void
PolarisSdma::Commit()
{
	// amdgpu_ring_commit(): pad with NOPs to the ring alignment (16 dwords)
	while ((fWptr & 15) != 0)
		Write(SDMA_PKT_HEADER_OP(SDMA_OP_NOP));

	// the ring is in write-combined VRAM: drain the CPU write buffers, then
	// the HDP write cache, before telling the engine
	__sync_synchronize();
	PolarisFlushHdp();
	WriteReg4AmdGpu(mmSDMA0_GFX_RB_WPTR, fWptr << 2);
}


status_t
PolarisSdma::WaitIdle(bigtime_t timeout)
{
	bigtime_t start = system_time();
	for (;;) {
		uint32 rptr = ReadReg4AmdGpu(mmSDMA0_GFX_RB_RPTR) >> 2;
		uint32 status = ReadReg4AmdGpu(mmSDMA0_STATUS_REG);
		if (rptr == fWptr && (status & SDMA0_STATUS_REG__IDLE_MASK) != 0)
			return B_OK;
		if (system_time() - start > timeout)
			return B_TIMED_OUT;
		snooze(10);
	}
}


void
PolarisSdma::PrintState()
{
	PolarisInvalidateHdp();
	printf("  SDMA0_STATUS_REG %#010" B_PRIx32 ", STATUS1 %#010" B_PRIx32
		", F32_CNTL %#010" B_PRIx32 "\n", ReadReg4AmdGpu(mmSDMA0_STATUS_REG),
		ReadReg4AmdGpu(mmSDMA0_STATUS1_REG), ReadReg4AmdGpu(mmSDMA0_F32_CNTL));
	printf("  RB_CNTL %#010" B_PRIx32 ", RB_RPTR %#" B_PRIx32 " (write-back %#"
		B_PRIx32 "), RB_WPTR %#" B_PRIx32 ", our wptr %#" B_PRIx32 "\n",
		ReadReg4AmdGpu(mmSDMA0_GFX_RB_CNTL), ReadReg4AmdGpu(mmSDMA0_GFX_RB_RPTR),
		*(volatile uint32*)fRptr.adr, ReadReg4AmdGpu(mmSDMA0_GFX_RB_WPTR),
		fWptr << 2);
	printf("  UCODE_ADDR %#" B_PRIx32 ", GRBM_STATUS %#010" B_PRIx32
		", SRBM_STATUS2 %#010" B_PRIx32 "\n",
		ReadReg4AmdGpu(mmSDMA0_UCODE_ADDR), ReadReg4AmdGpu(mmGRBM_STATUS),
		ReadReg4AmdGpu(mmSRBM_STATUS2));
}


void
PolarisSdma::EnableTrap(bool enable)
{
	uint32 value = ReadReg4AmdGpu(mmSDMA0_CNTL);
	WriteReg4AmdGpu(mmSDMA0_CNTL,
		SET_FIELD(value, SDMA0_CNTL, TRAP_ENABLE, enable ? 1 : 0));
}


void
PolarisSdma::EmitWrite(uint64 address, uint32 value)
{
	Write(SDMA_PKT_HEADER_OP(SDMA_OP_WRITE)
		| SDMA_PKT_HEADER_SUB_OP(SDMA_SUBOP_WRITE_LINEAR));
	Write((uint32)address);
	Write((uint32)(address >> 32));
	Write(SDMA_PKT_WRITE_UNTILED_DW_3_COUNT(1));
	Write(value);
}


void
PolarisSdma::EmitFence(uint64 address, uint32 value)
{
	Write(SDMA_PKT_HEADER_OP(SDMA_OP_FENCE));
	Write((uint32)address);
	Write((uint32)(address >> 32));
	Write(value);
}


void
PolarisSdma::EmitTrap(uint32 context)
{
	Write(SDMA_PKT_HEADER_OP(SDMA_OP_TRAP));
	Write(SDMA_PKT_TRAP_INT_CONTEXT_INT_CONTEXT(context));
}


void
PolarisSdma::EmitFill(uint64 address, uint32 value, uint32 bytes)
{
	// sdma_v3_0_emit_fill_buffer(), at most 0x3fffe0 bytes
	Write(SDMA_PKT_HEADER_OP(SDMA_OP_CONST_FILL));
	Write((uint32)address);
	Write((uint32)(address >> 32));
	Write(value);
	Write(bytes);
}


void
PolarisSdma::EmitCopy(uint64 source, uint64 destination, uint32 bytes)
{
	// sdma_v3_0_emit_copy_buffer(), at most 0x3fffe0 bytes
	Write(SDMA_PKT_HEADER_OP(SDMA_OP_COPY)
		| SDMA_PKT_HEADER_SUB_OP(SDMA_SUBOP_COPY_LINEAR));
	Write(bytes);
	Write(0);
	Write((uint32)source);
	Write((uint32)(source >> 32));
	Write((uint32)destination);
	Write((uint32)(destination >> 32));
}
