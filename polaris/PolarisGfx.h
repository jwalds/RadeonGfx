#pragma once

#include "RadeonMemory.h"


// GFX 8 command processor, ring 0 (Linux gfx_v8_0.c). The CE/PFP/ME/MEC and
// RLC firmware must have been loaded by the SMU.
class PolarisGfx {
public:
	PolarisGfx();
	~PolarisGfx();

	status_t Init();
	void Fini();

	status_t Begin(uint32 dwords);
	void Write(uint32 dword);
	void Commit();
	status_t WaitIdle(bigtime_t timeout);
	void PrintState();
	void PrintVmFaults();
	// valid waves in the shader engines (gfx_v8_0_read_wave_data())
	void DumpWaves(uint32 maxWaves = 4);

	void EnableEopInterrupt(bool enable);
	// gfx_v8_0_constants_init(): shader memory configuration for VMID 0
	void SetupShaderMemory();

	// packets
	void EmitSetUconfigReg(uint32 reg, uint32 value);
	void EmitWriteData(uint64 address, uint32 value);
	void EmitCopyData(uint64 source, uint64 destination,
		bool throughL2);
	void EmitFence(uint64 address, uint32 value, bool interrupt);
	// compute on the graphics ring (shader type bit set)
	void EmitSetComputeReg(uint32 reg, const uint32 *values, uint32 count);
	void EmitSetComputeReg(uint32 reg, uint32 value)
		{ EmitSetComputeReg(reg, &value, 1); }
	void EmitDispatch(uint32 x, uint32 y, uint32 z);
	void EmitCsPartialFlush();

private:
	void EmitClearState();

	MappedBuffer fRing;
	MappedBuffer fRptr;
	uint32 fRingDwords;
	uint32 fWptr;
	bool fRegistersSaved;
};
