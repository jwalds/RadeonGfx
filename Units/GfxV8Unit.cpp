#include "GfxV8Unit.h"
#include "RadeonInterrupts.h"
#include "RadeonDevice.h"
#include "polaris/PolarisGfx.h"
#include "polaris/PolarisGfxPackets.h"
#include "polaris/PolarisSdma.h"

#include <stdio.h>
#include <new>


// one dword NOP (count 0x3fff), as Linux gfx_v8_0_ring_funcs_gfx.nop
static const uint32 kNopPacket = PACKET3(PACKET3_NOP, 0x3fff);
static const uint32 kRingSize = 64 * 1024;


class RadeonRingBufferGfxV8 final: public RadeonRingBuffer {
private:
	PolarisGfx fGfx;

protected:
	status_t Start() override;
	status_t Stop() override;
	// fetching the ring from system memory stalled (UC: no data; default
	// MTYPE: CE/DE stuck); the bring-up tests ran it from VRAM
	MemoryDomain RingDomain() const override {return boDomainVramMappable;}

public:
	RadeonRingBufferGfxV8(RingType type);
	virtual ~RadeonRingBufferGfxV8();

	uint32 NopPacket() const override {return kNopPacket;}
	uint32 Rptr() override;
	uint32 Wptr() override;
	void SetWptr(uint32 val) override;
	void WriteFence2() override;
	void WriteUserFence(uint64 adr, uint64 seq) override;
	void WriteIb(uint64 adr, uint32 count, uint32 vmId) override;
	void WriteVmFlush(uint32 vmId, uint64 pdAdr) override;

	void EnableEopInterrupt(bool enable) {fGfx.EnableEopInterrupt(enable);}
	// halts the CP and restores the registers (PolarisGfx::Fini())
	void Shutdown() {Stop();}
	// the ring in system memory works: register write, memory write, read
	// pointer write-back, fence
	status_t SelfTest();
	void PrintState() {fGfx.PrintState();}
	void PrintVmFaults() {fGfx.PrintVmFaults();}

	void PrintHangState() override;
	bool SoftRecover(int32 vmId, uint32 seq) override;
	status_t ResetHardware() override;
};


RadeonRingBufferGfxV8::RadeonRingBufferGfxV8(RingType type):
	RadeonRingBuffer(type)
{
}


RadeonRingBufferGfxV8::~RadeonRingBufferGfxV8()
{
}


status_t
RadeonRingBufferGfxV8::Start()
{
	CheckRet(fGfx.InitHardware(fBuffer.buf->gpuPhysAdr, fSize, fRptrGpuAdr));
	CheckRet(Begin(1024));
	GenClearState(*this);
	End();
	printf("GFX:       ring %" B_PRIu32 " dwords at %#" B_PRIx64 ", CP running\n",
		fSize, fBuffer.buf->gpuPhysAdr);
	return B_OK;
}


status_t
RadeonRingBufferGfxV8::Stop()
{
	fGfx.Fini();
	return B_OK;
}


uint32
RadeonRingBufferGfxV8::Rptr()
{
	return *fRptrAdr * 4;
}


uint32
RadeonRingBufferGfxV8::Wptr()
{
	return ReadReg4AmdGpu(mmCP_RB0_WPTR) * 4;
}


void
RadeonRingBufferGfxV8::SetWptr(uint32 val)
{
	__sync_synchronize();
	// the ring is written through the BAR
	PolarisFlushHdp();
	WriteReg4AmdGpu(mmCP_RB0_WPTR, val / 4);
	ReadReg4AmdGpu(mmCP_RB0_WPTR);
}


void
RadeonRingBufferGfxV8::WriteFence2()
{
	GenFenceV8(*this, fFenceGpuAdr, Wseq(), false, true);
}


void
RadeonRingBufferGfxV8::WriteUserFence(uint64 adr, uint64 seq)
{
	GenFenceV8(*this, adr, seq, true, false);
}


void
RadeonRingBufferGfxV8::WriteIb(uint64 adr, uint32 count, uint32 vmId)
{
	GenIbV8(*this, adr, count, vmId);
}


void
RadeonRingBufferGfxV8::WriteVmFlush(uint32 vmId, uint64 pdAdr)
{
	GenVmFlushV8(*this, vmId, pdAdr);
}


status_t
RadeonRingBufferGfxV8::SelfTest()
{
	uint64 scratchGpu;
	void *scratchCpu;
	CheckRet(gDevice.MemMgr().Switch()->AllocWriteback(scratchGpu, scratchCpu,
		sizeof(uint32)));
	volatile uint32 *scratch = (volatile uint32*)scratchCpu;
	*scratch = 0;
	WriteReg4AmdGpu(mmSCRATCH_REG0, 0xcafedead);
	uint32 fenceBefore = *fFenceAdr;

	CheckRet(Begin(64));
	Write(PACKET3(PACKET3_SET_UCONFIG_REG, 1));
	Write(mmSCRATCH_REG0 - PACKET3_SET_UCONFIG_REG_START);
	Write(0xdeadbeef);
	Write(PACKET3(PACKET3_WRITE_DATA, 3));
	Write(WRITE_DATA_DST_SEL(5) | WR_CONFIRM | WRITE_DATA_ENGINE_SEL(0));
	Write((uint32)scratchGpu);
	Write((uint32)(scratchGpu >> 32));
	Write(0x12345678);
	GenFenceV8(*this, fFenceGpuAdr, 0x5e1f, false, false);
	End();

	bigtime_t start = system_time();
	while (*fFenceAdr != 0x5e1f && system_time() - start < 200000)
		snooze(100);
	bool ok = ReadReg4AmdGpu(mmSCRATCH_REG0) == 0xdeadbeef
		&& *scratch == 0x12345678 && *fFenceAdr == 0x5e1f;
	printf("GFX:       ring self test: scratch register %#" B_PRIx32
		", memory %#" B_PRIx32 ", fence %#" B_PRIx32 " (was %#" B_PRIx32
		"), rptr %#" B_PRIx32 " / register %#" B_PRIx32 ", wptr %#" B_PRIx32
		": %s\n", ReadReg4AmdGpu(mmSCRATCH_REG0), *scratch, *fFenceAdr,
		fenceBefore, *fRptrAdr, ReadReg4AmdGpu(mmCP_RB0_RPTR), fWptr,
		ok ? "OK" : "[!] FAILED");
	if (!ok) {
		fGfx.PrintState();
		fGfx.PrintVmFaults();
	}
	// the fence slot belongs to the ring's sequence numbers
	*fFenceAdr = fenceBefore;
	gDevice.MemMgr().Switch()->FreeWriteback(scratchGpu);
	return ok ? B_OK : B_ERROR;
}


void
RadeonRingBufferGfxV8::PrintHangState()
{
	printf("[!] GFX: ring rptr %#" B_PRIx32 " wptr %#" B_PRIx32 ", fence %"
		B_PRIu32 " of %" B_PRIu32 "\n", Rptr() / 4, Wptr() / 4, Rseq(),
		Wseq());
	fGfx.PrintState();
	fGfx.PrintVmFaults();
}


bool
RadeonRingBufferGfxV8::SoftRecover(int32 vmId, uint32 seq)
{
	// amdgpu_ring_soft_recovery(): kill the VMID's waves for up to 10 ms
	// until the fence passes; helps a shader that doesn't end, not a
	// command processor that waits
	if (vmId < 0)
		return false;
	bigtime_t deadline = system_time() + 10000;
	while ((int32)Rseq() - (int32)seq < 0 && system_time() < deadline) {
		fGfx.KillWaves(vmId);
		snooze(100);
	}
	return (int32)Rseq() - (int32)seq >= 0;
}


status_t
RadeonRingBufferGfxV8::ResetHardware()
{
	// amdgpu_device_gpu_recover() for the GFX block: what was on the ring is
	// dropped (RADV reports the device lost to every context anyway)
	fGfx.EnableEopInterrupt(false);
	CheckRet(fGfx.InitHardware(fBuffer.buf->gpuPhysAdr, fSize, fRptrGpuAdr,
		true));
	ResetPointers();
	for (uint32 i = 0; i < fSize; i++)
		((uint32*)fBuffer.adr)[i] = NopPacket();
	CheckRet(Begin(1024));
	GenClearState(*this);
	End();
	CheckRet(SelfTest());
	fGfx.EnableEopInterrupt(true);
	return B_OK;
}


// #pragma mark - GfxV8Unit

class GfxV8Unit: public GfxUnit {
private:
	ExternalUniquePtr<RadeonRingBuffer> fGfxRings[1];

protected:
	status_t InitHardware2() override;
	status_t FiniHardware2() override;

public:
	using GfxUnit::GfxUnit;

	virtual ~GfxV8Unit();
	const UnitInfo *GetInfo() override;

	uint32 GfxRingCount() override {return B_COUNT_OF(fGfxRings);}
	ExternalPtr<RadeonRingBuffer> *GfxRings() override
		{return (ExternalPtr<RadeonRingBuffer>*)fGfxRings;}
	uint32 ComputeRingCount() override {return 0;}
	ExternalPtr<RadeonRingBuffer> *ComputeRings() override {return NULL;}
};


GfxUnit *
GfxV8UnitNew(RadeonDevice *device)
{
	return new(std::nothrow) GfxV8Unit(device);
}


GfxV8Unit::~GfxV8Unit()
{
}


const UnitInfo *
GfxV8Unit::GetInfo()
{
	static const UnitInfo info {
		.type = UnitType::gfx,
		.version = {8, 0, 0},
		.name = "GFX v8.0"
	};
	return &info;
}


status_t
GfxV8Unit::InitHardware2()
{
	fGfxRings[0].SetTo(MakeExternal<RadeonRingBufferGfxV8>(
		RADEON_RING_TYPE_GFX_INDEX));
	if (!fGfxRings[0].IsSet())
		return B_NO_MEMORY;
	CheckRet(fGfxRings[0].Switch()->Init(kRingSize));
	Device()->InitRing(RADEON_RING_TYPE_GFX_INDEX, fGfxRings[0]);
	{
		auto ring = fGfxRings[0].Switch();
		CheckRet(static_cast<RadeonRingBufferGfxV8*>((RadeonRingBuffer*)ring)
			->SelfTest());
	}

	// CP end of pipe: fences of gfx ring 0
	gDevice.IntRing().Switch()->InstallHandler(0, intSrcIdCpEop,
		[](void *arg, InterruptPacket &packet) {
			(void)arg;
			if (packet.ringId == 0) {
				gDevice.Rings(RADEON_RING_TYPE_GFX_INDEX).Switch()
					->UpdateFences();
			}
		}, this);
	{
		auto ring = fGfxRings[0].Switch();
		static_cast<RadeonRingBufferGfxV8*>((RadeonRingBuffer*)ring)
			->EnableEopInterrupt(true);
	}
	return B_OK;
}


status_t
GfxV8Unit::FiniHardware2()
{
	if (!fGfxRings[0].IsSet())
		return B_OK;
	gDevice.IntRing().Switch()->UninstallHandler(0, intSrcIdCpEop);
	{
		auto ring = fGfxRings[0].Switch();
		auto gfxRing = static_cast<RadeonRingBufferGfxV8*>(
			(RadeonRingBuffer*)ring);
		if (gfxRing->Rseq() != gfxRing->Wseq()) {
			// unfinished work: what the GPU was doing
			printf("[!] GFX: fence %" B_PRIu32 " of %" B_PRIu32 " reached, ring"
				" rptr %#" B_PRIx32 " wptr %#" B_PRIx32 "\n", gfxRing->Rseq(),
				gfxRing->Wseq(), gfxRing->Rptr() / 4, gfxRing->Wptr() / 4);
			gfxRing->PrintState();
			gfxRing->PrintVmFaults();
		}
		gfxRing->EnableEopInterrupt(false);
		gfxRing->Shutdown();
	}
	return B_OK;
}
