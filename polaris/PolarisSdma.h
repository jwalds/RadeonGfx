#pragma once

#include "RadeonMemory.h"


// SDMA 3.0 engine 0 (Linux sdma_v3_0.c), firmware loaded directly through
// SDMA0_UCODE_ADDR/DATA (Linux' AMDGPU_FW_LOAD_DIRECT path). The ring and
// its read pointer write-back live in VRAM.
class PolarisSdma {
public:
	PolarisSdma();
	~PolarisSdma();

	status_t Init(const char *firmwarePath);
	void Fini();

	// packet building; Commit() pads to 16 dwords and kicks the engine
	status_t Begin(uint32 dwords);
	void Write(uint32 dword);
	void Commit();

	// waits until the engine has fetched everything committed
	status_t WaitIdle(bigtime_t timeout);
	void PrintState();

	void EnableTrap(bool enable);

	// packets
	void EmitWrite(uint64 address, uint32 value);
	void EmitFence(uint64 address, uint32 value);
	void EmitTrap(uint32 context);
	void EmitFill(uint64 address, uint32 value, uint32 bytes);
	void EmitCopy(uint64 source, uint64 destination, uint32 bytes);

	uint32 FirmwareVersion() {return fFirmwareVersion;}

private:
	MappedBuffer fRing;
	MappedBuffer fRptr;
	uint32 fRingDwords;
	uint32 fWptr;
	uint32 fFirmwareVersion;
	bool fRegistersSaved;
	bool fRunning;
};


// HDP: CPU accesses to VRAM go through the host data path
void PolarisFlushHdp();
void PolarisInvalidateHdp();
