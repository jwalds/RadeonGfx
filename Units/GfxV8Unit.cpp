#include "GfxV8Unit.h"
#include "RadeonInterrupts.h"
#include "RadeonDevice.h"
#include "polaris/PolarisGfx.h"
#include "polaris/PolarisGfxPackets.h"

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
	void PrintState() {fGfx.PrintState();}
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
		gfxRing->EnableEopInterrupt(false);
		gfxRing->Shutdown();
	}
	return B_OK;
}
