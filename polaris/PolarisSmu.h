#pragma once

#include "RadeonMemory.h"


// SMU 7 (Polaris) as in Linux smu7_smumgr.c/polaris10_smumgr.c: start the
// SMC firmware and have it load engine firmware (direct loading through the
// engines' UCODE registers is locked on Polaris).
class PolarisSmu {
public:
	struct Ucode {
		uint16 id;				// UCODE_ID_*
		const char *file;		// amdgpu firmware file
	};

	PolarisSmu();
	~PolarisSmu();

	bool IsFirmwareRunning();
	// starts polaris11_smc.bin (protection or non-protection mode)
	status_t Start(const char *firmwarePath);
	// loads the given ucodes through the SMU (PPSMC_MSG_LoadUcodes)
	status_t LoadUcodes(const char *firmwareDir, const Ucode *ucodes,
		uint32 count);
	// SDMA, CP and RLC firmware in one load, skipped if already loaded
	status_t LoadAllFirmware(const char *firmwareDir);

	uint32 Temperature();	// degrees Celsius
	void PrintState();

	uint32 ReadIndirect(uint32 address);
	void WriteIndirect(uint32 address, uint32 value);
	status_t SendMessage(uint16 message, uint32 parameter, uint32 *result
		= NULL);

private:
	status_t WaitIndirect(uint32 address, uint32 mask, uint32 value,
		bool equal, const char *what);

	MappedBuffer fTocBuffer;
	MappedBuffer fSmuBuffer;
	MappedBuffer fImages;
	uint32 fLoadedMask = 0;
	bool fStarted = false;
};


// UCODE_ID_* (Linux smu_ucode_xfer_vi.h)
enum {
	UCODE_ID_SDMA0 = 1,
	UCODE_ID_SDMA1 = 2,
	UCODE_ID_CP_CE = 3,
	UCODE_ID_CP_PFP = 4,
	UCODE_ID_CP_ME = 5,
	UCODE_ID_CP_MEC = 6,
	UCODE_ID_CP_MEC_JT1 = 7,
	UCODE_ID_CP_MEC_JT2 = 8,
	UCODE_ID_RLC_G = 10,
};
