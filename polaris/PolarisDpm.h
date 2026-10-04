#pragma once

#include <SupportDefs.h>

#include "vi/smu74_discrete.h"

class Atombios;
class PolarisPowerPlay;
class PolarisSmu;


// SMU 7 dynamic power management of a Polaris card as in Linux
// smu7_hwmgr.c and polaris10_smumgr.c: the SMC's DPM table is built from the
// VBIOS PowerPlay table, uploaded to SMC RAM and the SMC then switches the
// engine and memory clocks (and the VDDC voltage over SVI2) by GPU load.
//
// Not used: AVFS, clock stretching, BAPM/power containment, deep sleep, ULV
// and PCIe link DPM; the VBIOS EVV voltages are used as they are.
class PolarisDpm {
public:
	enum VoltageControl {
		kVoltageControlNone,
		kVoltageControlGpio,
		kVoltageControlSvi2,
	};

	PolarisDpm(PolarisSmu &smu, PolarisPowerPlay &powerPlay,
		Atombios &atombios, const uint8 *rom, size_t romSize);

	// VBIOS and SMC firmware information; reads registers and SMC RAM only
	status_t Init();
	void Print();

	// the SMU74 DPM table in memory; runs only ATOM clock calculations
	status_t BuildTable();
	void PrintTable();

	// MC arbiter timings (through the VBIOS) and the DPM table into SMC RAM
	status_t Upload();

	// enables engine clock DPM and, if memoryDpm, memory clock DPM
	status_t Start(bool memoryDpm);

	bool IsRunning();

private:
	struct BootState {
		uint32 sclk, mclk;			// 10 kHz
		uint16 vddc, vddci, mvdd;	// mV
		uint8 pcieGen;				// 0: gen 1
		uint8 pcieLanes;
	};

	struct SclkDividers {
		uint16 fcwInt, fcwFrac;
		uint8 postDiv, vcoMode, pllRange, sscEnable;
		uint16 ssFcw1Int, ssFcw1Frac;
		uint16 pccFcwInt, ssFcwSlewFrac, pccFcwSlewFrac;
	};

	const void *DataTable(uint32 index, uint8 *formatRevision = NULL,
		uint8 *contentRevision = NULL);
	bool HasVoltageObject(uint8 type, uint8 mode);
	bool GpioPin(uint8 id, uint8 &bitShift);

	status_t ComputeSclkDividers(uint32 clock, SclkDividers &dividers);
	status_t ComputeDfsDivider(uint32 clock, uint8 &postDivider,
		uint32 *realClock = NULL);
	status_t SetEngineDramTimings(uint32 sclk, uint32 mclk);
	status_t SetAcTiming(uint32 mclk, uint8 level);

	uint32 ReadPcie(uint32 address);
	status_t ReadSmcDword(uint32 address, uint32 &value);
	status_t CopyToSmc(uint32 address, const void *data, size_t size);

	SMU_VoltageLevel MinVoltage(uint32 vddc);
	status_t SclkSetting(uint32 clock, SMU_SclkSetting &setting);
	status_t BuildGraphicsLevels();
	void BuildMemoryLevels();
	status_t BuildLinkLevels();
	status_t BuildAcpiLevel();
	status_t BuildMmLevels();
	void BuildBootLevel();
	status_t BuildArbTable();
	void SwitchArbSet(uint32 source, uint32 destination);

	PolarisSmu &fSmu;
	PolarisPowerPlay &fPowerPlay;
	Atombios &fAtombios;
	const uint8 *fRom;
	size_t fRomSize;

	BootState fBoot{};
	VoltageControl fVddcControl = kVoltageControlNone;
	VoltageControl fVddciControl = kVoltageControlNone;
	VoltageControl fMvddControl = kVoltageControlNone;
	bool fGddr5 = false;
	uint32 fVramWidth = 0;
	uint8 fSclkRangeCount = 0;
	sclkFcwRange_t fSclkRange[NUM_SCLK_RANGE]{};
	uint8 fVrHotGpio = 0x7f, fAcDcGpio = 0x7f, fThermOutGpio = 0x7f;

	uint32 fSmcVersion = 0;
	uint32 fDpmTableStart = 0;
	uint32 fSoftRegsStart = 0;
	uint32 fArbTableStart = 0;

	uint32 fSclkCount = 0, fMclkCount = 0, fPcieCount = 0;
	uint32 fSclkEnableMask = 0, fMclkEnableMask = 0;
	SMU74_Discrete_DpmTable fTable{};
	SMU74_Discrete_MCArbDramTimingTable fArbTable{};
	bool fTableBuilt = false;
};
