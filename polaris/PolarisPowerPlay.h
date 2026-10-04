#pragma once

#include <SupportDefs.h>

typedef struct atom_context_s atom_context;


// The VBIOS PowerPlay table of a Polaris card (ATOM_Tonga_POWERPLAYTABLE,
// Linux process_pptables_v1_0.c): the DPM levels and their voltages.
class PolarisPowerPlay {
public:
	enum {
		kMaxLevels = 16,
	};

	struct SclkLevel {
		uint32 clock;		// 10 kHz
		uint16 vddc;		// mV, or a virtual voltage ID before ResolveVoltages
		uint8 vddcIndex;	// into the VDDC lookup table
		bool cksEnable;		// clock stretcher allowed at this level
	};

	struct MclkLevel {
		uint32 clock;		// 10 kHz
		uint16 vddc;		// mV
		uint8 vddcIndex;
		uint16 vddci;		// mV
		uint16 mvdd;		// mV
	};

	struct PcieLevel {
		uint8 gen;			// 0: gen 1, 1: gen 2, 2: gen 3
		uint8 lanes;
		uint32 sclk;		// 10 kHz, the BIF clock for this level
	};

	struct MmLevel {
		uint16 vddc;		// mV
		uint8 vddcIndex;
		uint32 dclk, vclk, eclk, aclk, samclk;	// 10 kHz
	};

	status_t Init(const uint8 *rom, size_t size);
	status_t ResolveVoltages(atom_context *atom);
	void Print();

	uint32 SclkLevelCount() const {return fSclkCount;}
	const SclkLevel &Sclk(uint32 i) const {return fSclk[i];}
	uint32 MclkLevelCount() const {return fMclkCount;}
	const MclkLevel &Mclk(uint32 i) const {return fMclk[i];}
	uint32 PcieLevelCount() const {return fPcieCount;}
	const PcieLevel &Pcie(uint32 i) const {return fPcie[i];}
	uint32 MmLevelCount() const {return fMmCount;}
	const MmLevel &Mm(uint32 i) const {return fMm[i];}
	uint32 VddcCount() const {return fVddcCount;}
	uint16 Vddc(uint32 i) const {return fVddc[i];}
	uint16 VddcCacLow(uint32 i) const {return fVddcCac[i][0];}
	uint16 VddcCacMid(uint32 i) const {return fVddcCac[i][1];}
	uint16 VddcCacHigh(uint32 i) const {return fVddcCac[i][2];}

	uint32 PlatformCaps() const {return fPlatformCaps;}
	uint16 UlvVoltageOffset() const {return fUlvOffset;}
	uint16 ClockStretchAmount() const {return fClockStretchAmount;}
	uint16 TjMax() const {return fTjMax;}				// degrees C
	uint16 Tdc() const {return fTdc;}					// A
	uint16 PowerTuneDataSetId() const {return fPowerTuneDataSetId;}
	uint8 CksLdoRefSel() const {return fCksLdoRefSel;}
	uint8 VrHotSclkLevel() const {return fVrHotSclkLevel;}

private:
	void UpdateLevelVoltages();

	uint8 fTableRevision = 0;
	uint32 fPlatformCaps = 0;
	uint32 fMaxOdSclk = 0, fMaxOdMclk = 0;
	uint16 fPowerLimit = 0;
	uint16 fUlvOffset = 0;
	uint16 fClockStretchAmount = 0;
	uint16 fTjMax = 0, fTdc = 0, fPowerTuneDataSetId = 0;
	uint8 fCksLdoRefSel = 0;
	uint8 fVrHotSclkLevel = 0;
	uint32 fSclkCount = 0, fMclkCount = 0, fVddcCount = 0;
	uint32 fPcieCount = 0, fMmCount = 0;
	SclkLevel fSclk[kMaxLevels];
	MclkLevel fMclk[kMaxLevels];
	PcieLevel fPcie[kMaxLevels];
	MmLevel fMm[kMaxLevels];
	uint16 fVddc[32];
	uint16 fVddcCac[32][3];
};
