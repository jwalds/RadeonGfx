#pragma once

#include <SupportDefs.h>


// The VBIOS PowerPlay table of a Polaris card (ATOM_Tonga_POWERPLAYTABLE,
// Linux process_pptables_v1_0.c): the DPM levels and their voltages.
class PolarisPowerPlay {
public:
	enum {
		kMaxLevels = 16,
	};

	struct SclkLevel {
		uint32 clock;		// 10 kHz
		uint16 vddc;		// mV (from the VDDC lookup table)
		bool enabled;
	};

	struct MclkLevel {
		uint32 clock;		// 10 kHz
		uint16 vddc;		// mV
		uint16 vddci;		// mV
		uint16 mvdd;		// mV
	};

	status_t Init(const uint8 *rom, size_t size);
	void Print();

	uint32 SclkLevelCount() const {return fSclkCount;}
	const SclkLevel &Sclk(uint32 i) const {return fSclk[i];}
	uint32 MclkLevelCount() const {return fMclkCount;}
	const MclkLevel &Mclk(uint32 i) const {return fMclk[i];}

private:
	uint8 fTableRevision = 0;
	uint32 fPlatformCaps = 0;
	uint32 fMaxOdSclk = 0, fMaxOdMclk = 0;
	uint16 fPowerLimit = 0;
	uint32 fSclkCount = 0, fMclkCount = 0, fVddcCount = 0;
	SclkLevel fSclk[kMaxLevels];
	MclkLevel fMclk[kMaxLevels];
	uint16 fVddc[32];
};
