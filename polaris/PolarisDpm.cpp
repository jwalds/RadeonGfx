#include "PolarisDpm.h"
#include "PolarisPowerPlay.h"
#include "PolarisSmu.h"
#include "Atombios.h"
#include "Radeon.h"

#include <stdio.h>
#include <stddef.h>
#include <string.h>
#include <ByteOrder.h>
#include <OS.h>

#include "atombios/atombios.h"
#include "vi/smu7_ppsmc.h"
#include "vi/smu_7_1_3_d.h"
#include "vi/smu_7_1_3_sh_mask.h"
#include "vi/gmc_8_1_d.h"
#include "vi/gmc_8_1_sh_mask.h"
#include "vi/bif_5_0_d.h"
#include "vi/bif_5_0_sh_mask.h"
#include "vi/pptable_v1_0.h"

#define CheckRet(err) {status_t _err = (err); if (_err < B_OK) return _err;}

// Linux smu7_hwmgr.c, smu7_dyn_defaults.h, hwmgr.h, polaris10_smumgr.c
#define ixPWR_SVI2_PLANE1_LOAD					0xc0200280
#define PWR_SVI2_PLANE1_LOAD__PSI1_MASK			0x00000020
#define PWR_SVI2_PLANE1_LOAD__PSI0_EN_MASK		0x00000040

#define MC_CG_ARB_FREQ_F0			0x0a
#define MC_CG_ARB_FREQ_F1			0x0b

#define SMC_RAM_END					0x40000

#define VOLTAGE_SCALE				4
#define VDDC_VDDCI_DELTA			200

#define SMU7_VOTINGRIGHTSCLIENTS_DFLT0	0x3fffc102
#define SMU7_VOTINGRIGHTSCLIENTS_DFLT1	0x000400
#define SMU7_VOTINGRIGHTSCLIENTS_DFLT2	0xc00080
#define SMU7_VOTINGRIGHTSCLIENTS_DFLT3	0xc00200
#define SMU7_VOTINGRIGHTSCLIENTS_DFLT4	0xc01680
#define SMU7_VOTINGRIGHTSCLIENTS_DFLT5	0xc00033
#define SMU7_VOTINGRIGHTSCLIENTS_DFLT6	0xc00033
#define SMU7_VOTINGRIGHTSCLIENTS_DFLT7	0x3fffc000
#define SMU7_STATICSCREENTHRESHOLDUNIT_DFLT	0
#define SMU7_STATICSCREENTHRESHOLD_DFLT		0x00c8
#define SMU7_SCLK_TARGETACTIVITY_DFLT		30

#define DISPLAY_GAP_VBLANK_OR_WM	0
#define DISPLAY_GAP_VBLANK			1
#define DISPLAY_GAP_IGNORE			3

#define DPM_EVENT_SRC_DIGITAL		2

#define SMU7_UNUSED_GPIO_PIN		0x7f
#define SMU7_Q88_FORMAT_CONVERSION_UNIT	256

static const uint32 kVotingRights[8] = {
	SMU7_VOTINGRIGHTSCLIENTS_DFLT0, SMU7_VOTINGRIGHTSCLIENTS_DFLT1,
	SMU7_VOTINGRIGHTSCLIENTS_DFLT2, SMU7_VOTINGRIGHTSCLIENTS_DFLT3,
	SMU7_VOTINGRIGHTSCLIENTS_DFLT4, SMU7_VOTINGRIGHTSCLIENTS_DFLT5,
	SMU7_VOTINGRIGHTSCLIENTS_DFLT6, SMU7_VOTINGRIGHTSCLIENTS_DFLT7
};

// pp_r600_encode_lanes (Linux pppcielanes.h)
static const uint8 kEncodedLanes[17] = {
	0, 1, 2, 0, 3, 0, 0, 0, 4, 0, 0, 0, 5, 0, 0, 0, 6
};

// PCIE_LC_LINK_WIDTH_CNTL.LC_LINK_WIDTH_RD
static const uint8 kLinkWidths[8] = {0, 1, 2, 4, 8, 12, 16, 0};

// MC_SHARED_CHMAP.NOOFCHAN (Linux gmc_v8_0.c)
static const uint8 kMemoryChannels[16] = {
	1, 2, 4, 8, 3, 6, 10, 12, 16, 0, 0, 0, 0, 0, 0, 0
};


// the SMC is big endian
static inline uint32 SmcUl(uint32 value) {return B_HOST_TO_BENDIAN_INT32(value);}
static inline uint16 SmcUs(uint16 value) {return B_HOST_TO_BENDIAN_INT16(value);}
static inline uint32 HostUl(uint32 value) {return B_BENDIAN_TO_HOST_INT32(value);}
static inline uint16 HostUs(uint16 value) {return B_BENDIAN_TO_HOST_INT16(value);}


PolarisDpm::PolarisDpm(PolarisSmu &smu, PolarisPowerPlay &powerPlay,
	Atombios &atombios, const uint8 *rom, size_t romSize)
	:
	fSmu(smu),
	fPowerPlay(powerPlay),
	fAtombios(atombios),
	fRom(rom),
	fRomSize(romSize)
{
}


// #pragma mark - VBIOS


const void *
PolarisDpm::DataTable(uint32 index, uint8 *formatRevision,
	uint8 *contentRevision)
{
	if (fRomSize < OFFSET_TO_POINTER_TO_ATOM_ROM_HEADER + 2)
		return NULL;
	uint16 romHeaderOffset
		= *(const uint16*)(fRom + OFFSET_TO_POINTER_TO_ATOM_ROM_HEADER);
	if (romHeaderOffset + sizeof(ATOM_ROM_HEADER) > fRomSize)
		return NULL;
	const ATOM_ROM_HEADER *romHeader
		= (const ATOM_ROM_HEADER*)(fRom + romHeaderOffset);
	uint16 masterOffset = romHeader->usMasterDataTableOffset;
	if (masterOffset + sizeof(ATOM_MASTER_DATA_TABLE) > fRomSize)
		return NULL;
	const ATOM_MASTER_DATA_TABLE *master
		= (const ATOM_MASTER_DATA_TABLE*)(fRom + masterOffset);
	if (index >= sizeof(master->ListOfDataTables) / sizeof(uint16))
		return NULL;
	uint16 offset = ((const uint16*)&master->ListOfDataTables)[index];
	if (offset == 0 || offset + sizeof(ATOM_COMMON_TABLE_HEADER) > fRomSize)
		return NULL;
	const ATOM_COMMON_TABLE_HEADER *header
		= (const ATOM_COMMON_TABLE_HEADER*)(fRom + offset);
	if (offset + header->usStructureSize > fRomSize)
		return NULL;
	if (formatRevision != NULL)
		*formatRevision = header->ucTableFormatRevision;
	if (contentRevision != NULL)
		*contentRevision = header->ucTableContentRevision;
	return header;
}


// atomctrl_is_voltage_controlled_by_gpio_v3()
bool
PolarisDpm::HasVoltageObject(uint8 type, uint8 mode)
{
	const uint8 *start = (const uint8*)DataTable(
		GetIndexIntoMasterTable(DATA, VoltageObjectInfo));
	if (start == NULL)
		return false;
	const ATOM_VOLTAGE_OBJECT_INFO_V3_1 *info
		= (const ATOM_VOLTAGE_OBJECT_INFO_V3_1*)start;
	size_t size = info->sHeader.usStructureSize;
	size_t offset = offsetof(ATOM_VOLTAGE_OBJECT_INFO_V3_1, asVoltageObj);
	while (offset + sizeof(ATOM_VOLTAGE_OBJECT_HEADER_V3) <= size) {
		const ATOM_VOLTAGE_OBJECT_HEADER_V3 *header
			= (const ATOM_VOLTAGE_OBJECT_HEADER_V3*)(start + offset);
		if (header->ucVoltageType == type && header->ucVoltageMode == mode)
			return true;
		if (header->usSize == 0)
			break;
		offset += header->usSize;
	}
	return false;
}


// atomctrl_get_pp_assign_pin()
bool
PolarisDpm::GpioPin(uint8 id, uint8 &bitShift)
{
	const uint8 *start = (const uint8*)DataTable(
		GetIndexIntoMasterTable(DATA, GPIO_Pin_LUT));
	if (start == NULL)
		return false;
	size_t size = ((const ATOM_COMMON_TABLE_HEADER*)start)->usStructureSize;
	size_t offset = offsetof(ATOM_GPIO_PIN_LUT, asGPIO_Pin);
	while (offset + sizeof(ATOM_GPIO_PIN_ASSIGNMENT) <= size) {
		const ATOM_GPIO_PIN_ASSIGNMENT *pin
			= (const ATOM_GPIO_PIN_ASSIGNMENT*)(start + offset);
		if (pin->ucGPIO_ID == id) {
			bitShift = pin->ucGpioPinBitShift;
			return true;
		}
		offset += sizeof(ATOM_GPIO_PIN_ASSIGNMENT);
	}
	return false;
}


// atomctrl_get_engine_pll_dividers_ai()
status_t
PolarisDpm::ComputeSclkDividers(uint32 clock, SclkDividers &dividers)
{
	union {
		uint32 raw[8];
		COMPUTE_GPU_CLOCK_OUTPUT_PARAMETERS_V1_7 out;
	} args = {};
	args.raw[0] = (clock & SET_CLOCK_FREQ_MASK)
		| (COMPUTE_GPUCLK_INPUT_FLAG_SCLK << 24);
	CheckRet(atom_execute_table(fAtombios.Context(),
		GetIndexIntoMasterTable(COMMAND, ComputeMemoryEnginePLL), args.raw));
	dividers.fcwFrac = args.out.usSclk_fcw_frac;
	dividers.fcwInt = args.out.usSclk_fcw_int;
	dividers.postDiv = args.out.ucSclkPostDiv;
	dividers.vcoMode = args.out.ucSclkVcoMode;
	dividers.pllRange = args.out.ucSclkPllRange;
	dividers.sscEnable = args.out.ucSscEnable;
	dividers.ssFcw1Frac = args.out.usSsc_fcw1_frac;
	dividers.ssFcw1Int = args.out.usSsc_fcw1_int;
	dividers.pccFcwInt = args.out.usPcc_fcw_int;
	dividers.ssFcwSlewFrac = args.out.usSsc_fcw_slew_frac;
	dividers.pccFcwSlewFrac = args.out.usPcc_fcw_slew_frac;
	return B_OK;
}


// atomctrl_get_dfs_pll_dividers_vi()
status_t
PolarisDpm::ComputeDfsDivider(uint32 clock, uint8 &postDivider,
	uint32 *realClock)
{
	uint32 args[8] = {};
	args[0] = (clock & SET_CLOCK_FREQ_MASK)
		| (COMPUTE_GPUCLK_INPUT_FLAG_DEFAULT_GPUCLK << 24);
	CheckRet(atom_execute_table(fAtombios.Context(),
		GetIndexIntoMasterTable(COMMAND, ComputeMemoryEnginePLL), args));
	postDivider = args[0] >> 24;
	if (realClock != NULL)
		*realClock = args[0] & 0xffffff;
	return B_OK;
}


// atomctrl_set_engine_dram_timings_rv770(): programs MC_ARB_DRAM_TIMING(2)
// and MC_ARB_BURST_TIME.STATE0 (the F0 arbiter set) for the clock pair
status_t
PolarisDpm::SetEngineDramTimings(uint32 sclk, uint32 mclk)
{
	uint32 args[8] = {};
	args[0] = (sclk & SET_CLOCK_FREQ_MASK) | (COMPUTE_ENGINE_PLL_PARAM << 24);
	args[1] = mclk & SET_CLOCK_FREQ_MASK;
	return atom_execute_table(fAtombios.Context(),
		GetIndexIntoMasterTable(COMMAND, DynamicMemorySettings), args);
}


// atomctrl_set_ac_timing_ai(): the MC sequencer settings of a memory level
status_t
PolarisDpm::SetAcTiming(uint32 mclk, uint8 level)
{
	uint32 args[8] = {};
	args[0] = (mclk & SET_CLOCK_FREQ_MASK) | (ADJUST_MC_SETTING_PARAM << 24);
	args[1] = level;
	return atom_execute_table(fAtombios.Context(),
		GetIndexIntoMasterTable(COMMAND, DynamicMemorySettings), args);
}


// #pragma mark - registers


uint32
PolarisDpm::ReadPcie(uint32 address)
{
	WriteReg4AmdGpu(mmPCIE_INDEX, address);
	ReadReg4AmdGpu(mmPCIE_INDEX);
	return ReadReg4AmdGpu(mmPCIE_DATA);
}


status_t
PolarisDpm::ReadSmcDword(uint32 address, uint32 &value)
{
	if ((address & 3) != 0 || address + 4 > SMC_RAM_END)
		return B_BAD_VALUE;
	value = fSmu.ReadIndirect(address);
	return B_OK;
}


// smu7_copy_bytes_to_smc(): SMC RAM holds big endian dwords
status_t
PolarisDpm::CopyToSmc(uint32 address, const void *data, size_t size)
{
	if ((address & 3) != 0 || address + size > SMC_RAM_END)
		return B_BAD_VALUE;
	const uint8 *bytes = (const uint8*)data;
	while (size >= 4) {
		uint32 value = ((uint32)bytes[0] << 24) | ((uint32)bytes[1] << 16)
			| ((uint32)bytes[2] << 8) | bytes[3];
		fSmu.WriteIndirect(address, value);
		address += 4;
		bytes += 4;
		size -= 4;
	}
	if (size > 0) {
		uint32 value = fSmu.ReadIndirect(address) & (0xffffffff >> (8 * size));
		for (size_t i = 0; i < size; i++)
			value |= (uint32)bytes[i] << (24 - 8 * i);
		fSmu.WriteIndirect(address, value);
	}
	return B_OK;
}


// #pragma mark - information


status_t
PolarisDpm::Init()
{
	const ATOM_FIRMWARE_INFO_V2_2 *firmwareInfo
		= (const ATOM_FIRMWARE_INFO_V2_2*)DataTable(
			GetIndexIntoMasterTable(DATA, FirmwareInfo));
	if (firmwareInfo == NULL) {
		printf("[!] DPM: no VBIOS firmware info\n");
		return B_BAD_DATA;
	}
	fBoot.sclk = firmwareInfo->ulDefaultEngineClock;
	fBoot.mclk = firmwareInfo->ulDefaultMemoryClock;
	fBoot.vddc = firmwareInfo->usBootUpVDDCVoltage;
	fBoot.vddci = firmwareInfo->usBootUpVDDCIVoltage;
	fBoot.mvdd = firmwareInfo->usBootUpMVDDCVoltage;

	// vi_get_xclk()
	fXclk = firmwareInfo->usCoreReferenceClock;
	if ((fSmu.ReadIndirect(ixCG_CLKPIN_CNTL_2)
			& CG_CLKPIN_CNTL_2__MUX_TCLK_TO_XCLK_MASK) != 0)
		fXclk = 1000;
	else if ((fSmu.ReadIndirect(ixCG_CLKPIN_CNTL)
			& CG_CLKPIN_CNTL__XTALIN_DIVIDE_MASK) != 0)
		fXclk /= 4;

	uint32 speed = ReadPcie(ixPCIE_LC_SPEED_CNTL);
	fBoot.pcieGen = (speed & PCIE_LC_SPEED_CNTL__LC_CURRENT_DATA_RATE_MASK)
		>> PCIE_LC_SPEED_CNTL__LC_CURRENT_DATA_RATE__SHIFT;
	uint32 width = ReadPcie(ixPCIE_LC_LINK_WIDTH_CNTL);
	fBoot.pcieLanes = kLinkWidths[(width
		& PCIE_LC_LINK_WIDTH_CNTL__LC_LINK_WIDTH_RD_MASK)
		>> PCIE_LC_LINK_WIDTH_CNTL__LC_LINK_WIDTH_RD__SHIFT];

	// smu7_init_dpm_defaults()
	if (HasVoltageObject(VOLTAGE_TYPE_VDDC, VOLTAGE_OBJ_SVID2))
		fVddcControl = kVoltageControlSvi2;
	else if (HasVoltageObject(VOLTAGE_TYPE_VDDC, VOLTAGE_OBJ_GPIO_LUT))
		fVddcControl = kVoltageControlGpio;
	uint32 caps = fPowerPlay.PlatformCaps();
	if ((caps & ATOM_TONGA_PP_PLATFORM_CAP_MVDD_CONTROL) != 0) {
		if (HasVoltageObject(VOLTAGE_TYPE_MVDDC, VOLTAGE_OBJ_GPIO_LUT))
			fMvddControl = kVoltageControlGpio;
		else if (HasVoltageObject(VOLTAGE_TYPE_MVDDC, VOLTAGE_OBJ_SVID2))
			fMvddControl = kVoltageControlSvi2;
	}
	if ((caps & ATOM_TONGA_PP_PLATFORM_CAP_VDDCI_CONTROL) != 0) {
		if (HasVoltageObject(VOLTAGE_TYPE_VDDCI, VOLTAGE_OBJ_GPIO_LUT))
			fVddciControl = kVoltageControlGpio;
		else if (HasVoltageObject(VOLTAGE_TYPE_VDDCI, VOLTAGE_OBJ_SVID2))
			fVddciControl = kVoltageControlSvi2;
	}

	fGddr5 = (ReadReg4AmdGpu(mmMC_SEQ_MISC0) >> 28) == 5;
	uint32 channelSize = (ReadReg4AmdGpu(mmMC_ARB_RAMCFG)
		& MC_ARB_RAMCFG__CHANSIZE_MASK) != 0 ? 64 : 32;
	uint32 channels = kMemoryChannels[(ReadReg4AmdGpu(mmMC_SHARED_CHMAP)
		& MC_SHARED_CHMAP__NOOFCHAN_MASK) >> MC_SHARED_CHMAP__NOOFCHAN__SHIFT];
	fVramWidth = channels * channelSize;

	// atomctrl_get_smc_sclk_range_table()
	const ATOM_SMU_INFO_V2_1 *smuInfo = (const ATOM_SMU_INFO_V2_1*)DataTable(
		GetIndexIntoMasterTable(DATA, SMU_Info));
	if (smuInfo == NULL) {
		printf("[!] DPM: no VBIOS SMU info\n");
		return B_BAD_DATA;
	}
	fSclkRangeCount = smuInfo->ucSclkEntryNum < NUM_SCLK_RANGE
		? smuInfo->ucSclkEntryNum : NUM_SCLK_RANGE;
	for (uint32 i = 0; i < fSclkRangeCount; i++) {
		const ATOM_SCLK_FCW_RANGE_ENTRY_V1 &entry
			= smuInfo->asSclkFcwRangeEntry[i];
		fSclkRange[i].vco_setting = entry.ucVco_setting;
		fSclkRange[i].postdiv = entry.ucPostdiv;
		fSclkRange[i].fcw_pcc = entry.ucFcw_pcc;
		fSclkRange[i].fcw_trans_upper = entry.ucFcw_trans_upper;
		fSclkRange[i].fcw_trans_lower = entry.ucRcw_trans_lower;
	}

	uint8 bitShift;
	if (GpioPin(VDDC_VRHOT_GPIO_PINID, bitShift))
		fVrHotGpio = bitShift;
	if (GpioPin(PP_AC_DC_SWITCH_GPIO_PINID, bitShift))
		fAcDcGpio = bitShift;
	if (GpioPin(THERMAL_INT_OUTPUT_GPIO_PINID, bitShift))
		fThermOutGpio = bitShift;

	// polaris10_process_firmware_header()
	if (!fSmu.IsFirmwareRunning()) {
		printf("[!] DPM: the SMC firmware isn't running\n");
		return B_NOT_INITIALIZED;
	}
	uint32 header = SMU7_FIRMWARE_HEADER_LOCATION;
	CheckRet(ReadSmcDword(header + offsetof(SMU74_Firmware_Header, Version),
		fSmcVersion));
	CheckRet(ReadSmcDword(header + offsetof(SMU74_Firmware_Header, DpmTable),
		fDpmTableStart));
	CheckRet(ReadSmcDword(header
		+ offsetof(SMU74_Firmware_Header, SoftRegisters), fSoftRegsStart));
	CheckRet(ReadSmcDword(header
		+ offsetof(SMU74_Firmware_Header, mcArbDramTimingTable),
		fArbTableStart));
	if (fDpmTableStart == 0 || fDpmTableStart >= SMC_RAM_END
		|| fSoftRegsStart == 0 || fSoftRegsStart >= SMC_RAM_END
		|| fArbTableStart == 0 || fArbTableStart >= SMC_RAM_END) {
		printf("[!] DPM: bad SMC firmware header\n");
		return B_BAD_DATA;
	}
	return B_OK;
}


static const char *
VoltageControlName(PolarisDpm::VoltageControl control)
{
	switch (control) {
		case PolarisDpm::kVoltageControlGpio:
			return "GPIO";
		case PolarisDpm::kVoltageControlSvi2:
			return "SVI2";
		default:
			return "none";
	}
}


void
PolarisDpm::Print()
{
	printf("DPM: boot engine clock %" B_PRIu32 " MHz, memory clock %" B_PRIu32
		" MHz, vddc %u mV, vddci %u mV, mvdd %u mV, PCIe gen %u x%u\n",
		fBoot.sclk / 100, fBoot.mclk / 100, fBoot.vddc, fBoot.vddci,
		fBoot.mvdd, fBoot.pcieGen + 1, fBoot.pcieLanes);
	printf("  voltage control: vddc %s, vddci %s, mvdd %s\n",
		VoltageControlName(fVddcControl), VoltageControlName(fVddciControl),
		VoltageControlName(fMvddControl));
	printf("  memory: %s, %" B_PRIu32 " bit; reference clock %" B_PRIu32
		" MHz\n", fGddr5 ? "GDDR5" : "not GDDR5", fVramWidth, fXclk / 100);
	printf("  GPIO pins: VR hot %u, AC/DC %u, thermal out %u\n", fVrHotGpio,
		fAcDcGpio, fThermOutGpio);
	for (uint32 i = 0; i < fSclkRangeCount; i++) {
		printf("  sclk range %" B_PRIu32 ": vco %u, postdiv %u, fcw pcc %#x,"
			" trans %#x..%#x\n", i, fSclkRange[i].vco_setting,
			fSclkRange[i].postdiv, fSclkRange[i].fcw_pcc,
			fSclkRange[i].fcw_trans_lower, fSclkRange[i].fcw_trans_upper);
	}
	printf("  SMC firmware %#" B_PRIx32 ": DPM table at %#" B_PRIx32 ", soft"
		" registers at %#" B_PRIx32 ", arbiter table at %#" B_PRIx32 "\n",
		fSmcVersion, fDpmTableStart, fSoftRegsStart, fArbTableStart);
}


// #pragma mark - DPM table


// polaris10_get_dependency_volt_by_clk() without VDDCI/MVDD control
SMU_VoltageLevel
PolarisDpm::MinVoltage(uint32 vddc)
{
	return (vddc * VOLTAGE_SCALE) << VDDC_SHIFT
		| (fBoot.vddci * VOLTAGE_SCALE) << VDDCI_SHIFT
		| 1 << PHASES_SHIFT;
}


// polaris10_calculate_sclk_params()
status_t
PolarisDpm::SclkSetting(uint32 clock, SMU_SclkSetting &setting)
{
	SclkDividers dividers;
	CheckRet(ComputeSclkDividers(clock, dividers));
	setting.SclkFrequency = SmcUl(clock);
	setting.Fcw_int = SmcUs(dividers.fcwInt);
	setting.Fcw_frac = SmcUs(dividers.fcwFrac);
	setting.Pcc_fcw_int = SmcUs(dividers.pccFcwInt);
	setting.PllRange = dividers.pllRange;
	setting.SSc_En = dividers.sscEnable;
	setting.Sclk_slew_rate = SmcUs(0x400);
	setting.Pcc_up_slew_rate = SmcUs(dividers.pccFcwSlewFrac);
	setting.Pcc_down_slew_rate = SmcUs(0xffff);
	setting.Fcw1_int = SmcUs(dividers.ssFcw1Int);
	setting.Fcw1_frac = SmcUs(dividers.ssFcw1Frac);
	setting.Sclk_ss_slew_rate = SmcUs(dividers.ssFcwSlewFrac);
	return B_OK;
}


// polaris10_populate_all_graphic_levels()
status_t
PolarisDpm::BuildGraphicsLevels()
{
	for (uint32 i = 0; i < fSclkCount; i++) {
		const PolarisPowerPlay::SclkLevel &sclk = fPowerPlay.Sclk(i);
		SMU74_Discrete_GraphicsLevel &level = fTable.GraphicsLevel[i];
		CheckRet(SclkSetting(sclk.clock, level.SclkSetting));
		level.MinVoltage = SmcUl(MinVoltage(sclk.vddc));
		level.ActivityLevel = SmcUs(SMU7_SCLK_TARGETACTIVITY_DFLT);
		level.CcPwrDynRm = 0;
		level.CcPwrDynRm1 = 0;
		level.EnabledForActivity = (fSclkEnableMask >> i) & 1;
		level.EnabledForThrottle = 1;
		level.UpHyst = 0;
		level.DownHyst = 100;
		level.VoltageDownHyst = 0;
		level.PowerThrottle = 0;
		level.DeepSleepDivId = 0;
		level.pcieDpmLevel = i < fPcieCount - 1 ? i : fPcieCount - 1;
	}
	fTable.GraphicsDpmLevelCount = fSclkCount;
	return B_OK;
}


// polaris10_populate_all_memory_levels()
void
PolarisDpm::BuildMemoryLevels()
{
	uint8 upHyst = 0, downHyst = 100, activity = 10;
	if (fVramWidth == 256) {
		upHyst = 10;
		downHyst = 60;
		activity = 25;
	} else if (fVramWidth == 128) {
		upHyst = 5;
		downHyst = 16;
		activity = 20;
	} else if (fVramWidth == 64) {
		upHyst = 3;
		downHyst = 16;
		activity = 20;
	}

	for (uint32 i = 0; i < fMclkCount; i++) {
		const PolarisPowerPlay::MclkLevel &mclk = fPowerPlay.Mclk(i);
		SMU74_Discrete_MemoryLevel &level = fTable.MemoryLevel[i];
		level.MinVoltage = SmcUl(MinVoltage(mclk.vddc));
		level.MinMvdd = SmcUl(fBoot.mvdd * VOLTAGE_SCALE);
		level.MclkFrequency = SmcUl(mclk.clock);
		level.EnabledForThrottle = 1;
		level.EnabledForActivity = (fMclkEnableMask >> i) & 1;
		level.UpHyst = upHyst;
		level.DownHyst = downHyst;
		level.VoltageDownHyst = 0;
		level.ActivityLevel = SmcUs(activity);
		level.StutterEnable = 0;
		level.DisplayWatermark = i == fMclkCount - 1
			? PPSMC_DISPLAY_WATERMARK_HIGH : PPSMC_DISPLAY_WATERMARK_LOW;
	}
	fTable.MemoryDpmLevelCount = fMclkCount;
}


// polaris10_populate_smc_link_level(); the PCIe link isn't switched (PCIe
// DPM stays disabled), its boot level is the current link
status_t
PolarisDpm::BuildLinkLevels()
{
	for (uint32 i = 0; i <= fPcieCount; i++) {
		SMU74_Discrete_LinkLevel &level = fTable.LinkLevel[i];
		uint8 gen = fBoot.pcieGen;
		uint8 lanes = fBoot.pcieLanes;
		if (i < fPcieCount) {
			const PolarisPowerPlay::PcieLevel &pcie = fPowerPlay.Pcie(i + 1);
			gen = pcie.gen < fBoot.pcieGen ? pcie.gen : fBoot.pcieGen;
			lanes = pcie.lanes < fBoot.pcieLanes ? pcie.lanes
				: fBoot.pcieLanes;
			uint8 divider;
			CheckRet(ComputeDfsDivider(pcie.sclk, divider));
			level.BifSclkDfs = SmcUs(divider);
		}
		level.PcieGenSpeed = gen;
		level.PcieLaneCount = lanes <= 16 ? kEncodedLanes[lanes] : 0;
		level.EnabledForActivity = 1;
		level.SPC = fBoot.pcieGen >= 2 ? 20 : 16;
		level.DownThreshold = SmcUl(5);
		level.UpThreshold = SmcUl(30);
	}
	fTable.LinkLevelCount = fPcieCount;
	fTable.PCIeBootLinkLevel = fPcieCount;
	fTable.PCIeGenInterval = 1;

	uint8 divider;
	CheckRet(ComputeDfsDivider(fPowerPlay.Pcie(0).sclk, divider));
	fTable.Ulv.BifSclkDfs = SmcUs(divider);
	return B_OK;
}


// polaris10_populate_smc_acpi_level()
status_t
PolarisDpm::BuildAcpiLevel()
{
	uint32 vddc = fPowerPlay.Sclk(fSclkCount - 1).vddc;
	for (uint32 i = 0; i < fSclkCount; i++) {
		if (fPowerPlay.Sclk(i).clock >= fBoot.sclk) {
			vddc = fPowerPlay.Sclk(i).vddc;
			break;
		}
	}
	SMU74_Discrete_ACPILevel &acpi = fTable.ACPILevel;
	acpi.Flags = 0;
	acpi.MinVoltage = SmcUl(MinVoltage(vddc));
	CheckRet(SclkSetting(fBoot.sclk, acpi.SclkSetting));
	acpi.DeepSleepDivId = 0;
	acpi.CcPwrDynRm = 0;
	acpi.CcPwrDynRm1 = 0;

	vddc = fPowerPlay.Mclk(fMclkCount - 1).vddc;
	for (uint32 i = 0; i < fMclkCount; i++) {
		if (fPowerPlay.Mclk(i).clock >= fBoot.mclk) {
			vddc = fPowerPlay.Mclk(i).vddc;
			break;
		}
	}
	SMU74_Discrete_MemoryLevel &memory = fTable.MemoryACPILevel;
	memory.MclkFrequency = SmcUl(fBoot.mclk);
	memory.MinVoltage = SmcUl(MinVoltage(vddc));
	memory.MinMvdd = 0;
	memory.StutterEnable = 0;
	memory.EnabledForThrottle = 0;
	memory.EnabledForActivity = 0;
	memory.UpHyst = 0;
	memory.DownHyst = 100;
	memory.VoltageDownHyst = 0;
	memory.ActivityLevel = SmcUs(SMU7_SCLK_TARGETACTIVITY_DFLT);
	return B_OK;
}


// polaris10_populate_smc_uvd_level(), _vce_level(), _samu_level()
status_t
PolarisDpm::BuildMmLevels()
{
	uint32 count = fPowerPlay.MmLevelCount();
	if (count > SMU74_MAX_LEVELS_UVD)
		count = SMU74_MAX_LEVELS_UVD;
	fTable.UvdLevelCount = count;
	fTable.VceLevelCount = count;
	fTable.SamuLevelCount = count;
	fTable.UvdBootLevel = 0;
	fTable.VceBootLevel = 0;
	fTable.SamuBootLevel = 0;
	for (uint32 i = 0; i < count; i++) {
		const PolarisPowerPlay::MmLevel &mm = fPowerPlay.Mm(i);
		SMU_VoltageLevel voltage = SmcUl(MinVoltage(mm.vddc));
		uint8 divider;

		SMU74_Discrete_UvdLevel &uvd = fTable.UvdLevel[i];
		uvd.VclkFrequency = SmcUl(mm.vclk);
		uvd.DclkFrequency = SmcUl(mm.dclk);
		uvd.MinVoltage = voltage;
		CheckRet(ComputeDfsDivider(mm.vclk, divider));
		uvd.VclkDivider = divider;
		CheckRet(ComputeDfsDivider(mm.dclk, divider));
		uvd.DclkDivider = divider;

		SMU74_Discrete_ExtClkLevel &vce = fTable.VceLevel[i];
		vce.Frequency = SmcUl(mm.eclk);
		vce.MinVoltage = voltage;
		CheckRet(ComputeDfsDivider(mm.eclk, divider));
		vce.Divider = divider;

		SMU74_Discrete_ExtClkLevel &samu = fTable.SamuLevel[i];
		samu.Frequency = SmcUl(mm.samclk);
		samu.MinVoltage = voltage;
		CheckRet(ComputeDfsDivider(mm.samclk, divider));
		samu.Divider = divider;
	}
	return B_OK;
}


// polaris10_populate_smc_boot_level(), _initailial_state()
void
PolarisDpm::BuildBootLevel()
{
	fTable.GraphicsBootLevel = 0;
	for (uint32 i = 0; i < fSclkCount; i++) {
		if (fPowerPlay.Sclk(i).clock >= fBoot.sclk) {
			fTable.GraphicsBootLevel = i;
			break;
		}
	}
	fTable.MemoryBootLevel = 0;
	for (uint32 i = 0; i < fMclkCount; i++) {
		if (fPowerPlay.Mclk(i).clock >= fBoot.mclk) {
			fTable.MemoryBootLevel = i;
			break;
		}
	}
	fTable.BootVddc = SmcUs(fBoot.vddc * VOLTAGE_SCALE);
	fTable.BootVddci = SmcUs(fBoot.vddci * VOLTAGE_SCALE);
	fTable.BootMVdd = SmcUs(fBoot.mvdd * VOLTAGE_SCALE);
}


status_t
PolarisDpm::BuildTable()
{
	fTableBuilt = false;
	memset(&fTable, 0, sizeof(fTable));

	if (fVddcControl != kVoltageControlSvi2
		|| fVddciControl != kVoltageControlNone
		|| fMvddControl != kVoltageControlNone) {
		printf("[!] DPM: only SVI2 VDDC control without VDDCI/MVDD control is"
			" supported\n");
		return B_NOT_SUPPORTED;
	}
	fSclkCount = fPowerPlay.SclkLevelCount();
	fMclkCount = fPowerPlay.MclkLevelCount();
	if (fSclkCount == 0 || fSclkCount > SMU74_MAX_LEVELS_GRAPHICS
		|| fMclkCount == 0 || fMclkCount > SMU74_MAX_LEVELS_MEMORY
		|| fPowerPlay.PcieLevelCount() < 2) {
		printf("[!] DPM: unsupported number of DPM levels\n");
		return B_NOT_SUPPORTED;
	}
	for (uint32 i = 0; i < fSclkCount; i++) {
		uint32 vddc = fPowerPlay.Sclk(i).vddc;
		if (vddc == 0 || vddc >= 1200 || (i > 0
			&& fPowerPlay.Sclk(i).clock <= fPowerPlay.Sclk(i - 1).clock)) {
			printf("[!] DPM: bad engine clock level %" B_PRIu32 "\n", i);
			return B_BAD_DATA;
		}
	}
	for (uint32 i = 0; i < fMclkCount; i++) {
		uint32 vddc = fPowerPlay.Mclk(i).vddc;
		if (vddc == 0 || vddc >= 1200 || (i > 0
			&& fPowerPlay.Mclk(i).clock <= fPowerPlay.Mclk(i - 1).clock)) {
			printf("[!] DPM: bad memory clock level %" B_PRIu32 "\n", i);
			return B_BAD_DATA;
		}
	}
	for (uint32 i = 0; i < fPowerPlay.MmLevelCount(); i++) {
		uint32 vddc = fPowerPlay.Mm(i).vddc;
		if (vddc == 0 || vddc >= 1200) {
			printf("[!] DPM: bad multimedia level %" B_PRIu32 "\n", i);
			return B_BAD_DATA;
		}
	}
	// the first PCIe table entry is for ULV, one link level is for booting
	fPcieCount = fPowerPlay.PcieLevelCount() - 1;
	if (fPcieCount > SMU74_MAX_LEVELS_LINK - 1)
		fPcieCount = SMU74_MAX_LEVELS_LINK - 1;
	fSclkEnableMask = (1 << fSclkCount) - 1;
	// switching the memory clock needs long enough vertical blanks, which
	// isn't checked (Linux smu7_vblank_too_short()): the memory clock is
	// switched once to its highest level
	fMclkEnableMask = 1 << (fMclkCount - 1);

	// polaris10_init_smc_table()
	// SVI2 VDDC: no SMIO tables (polaris10_populate_smc_voltage_tables()),
	// only the BAPM VIDs (polaris10_populate_cac_table())
	for (uint32 i = 0; i < fPowerPlay.VddcCount() && i < SMU74_MAX_LEVELS_VDDC;
			i++) {
		fTable.BapmVddcVidLoSidd[i]
			= (6200 - fPowerPlay.VddcCacLow(i) * VOLTAGE_SCALE) / 25;
		fTable.BapmVddcVidHiSidd[i]
			= (6200 - fPowerPlay.VddcCacMid(i) * VOLTAGE_SCALE) / 25;
		fTable.BapmVddcVidHiSidd2[i]
			= (6200 - fPowerPlay.VddcCacHigh(i) * VOLTAGE_SCALE) / 25;
	}

	uint32 systemFlags = 0;
	if (fGddr5)
		systemFlags |= PPSMC_SYSTEMFLAG_GDDR5;
	fTable.SystemFlags = SmcUl(systemFlags);

	// polaris10_get_sclk_range_table()
	for (uint32 i = 0; i < fSclkRangeCount; i++) {
		fTable.SclkFcwRangeTable[i].vco_setting = fSclkRange[i].vco_setting;
		fTable.SclkFcwRangeTable[i].postdiv = fSclkRange[i].postdiv;
		fTable.SclkFcwRangeTable[i].fcw_pcc = SmcUs(fSclkRange[i].fcw_pcc);
		fTable.SclkFcwRangeTable[i].fcw_trans_upper
			= SmcUs(fSclkRange[i].fcw_trans_upper);
		fTable.SclkFcwRangeTable[i].fcw_trans_lower
			= SmcUs(fSclkRange[i].fcw_trans_lower);
	}

	CheckRet(BuildLinkLevels());
	CheckRet(BuildGraphicsLevels());
	BuildMemoryLevels();
	CheckRet(BuildAcpiLevel());
	CheckRet(BuildMmLevels());
	BuildBootLevel();

	fTable.CurrSclkPllRange = SmcUl(0xff);
	fTable.GraphicsVoltageChangeEnable = 1;
	fTable.GraphicsThermThrottleEnable = 1;
	fTable.GraphicsInterval = 1;
	fTable.VoltageInterval = 1;
	fTable.ThermalInterval = 1;
	fTable.TemperatureLimitHigh = SmcUs(fPowerPlay.TjMax()
		* SMU7_Q88_FORMAT_CONVERSION_UNIT);
	fTable.TemperatureLimitLow = SmcUs((fPowerPlay.TjMax() - 1)
		* SMU7_Q88_FORMAT_CONVERSION_UNIT);
	fTable.MemoryVoltageChangeEnable = 1;
	fTable.MemoryInterval = 1;
	fTable.VoltageResponseTime = 0;
	fTable.PhaseResponseTime = 0;
	fTable.MemoryThermThrottleEnable = 1;

	// polaris10_populate_vr_config(): VDDGFX merged with VDDC, VDDC on SVI2
	// plane 1, VDDCI and MVDD static
	fTable.VRConfig = SmcUl(VR_MERGED_WITH_VDDC << VRCONF_VDDGFX_SHIFT
		| VR_SVI2_PLANE_1 << VRCONF_VDDC_SHIFT
		| VR_STATIC_VOLTAGE << VRCONF_VDDCI_SHIFT
		| VR_STATIC_VOLTAGE << VRCONF_MVDD_SHIFT);
	fTable.ThermGpio = 17;
	fTable.SclkStepSize = SmcUl(0x4000);

	if (fVrHotGpio != SMU7_UNUSED_GPIO_PIN) {
		fTable.VRHotGpio = fVrHotGpio;
		fTable.VRHotLevel = fPowerPlay.VrHotSclkLevel();
	} else
		fTable.VRHotGpio = SMU7_UNUSED_GPIO_PIN;
	fTable.AcDcGpio = fAcDcGpio;
	if (fThermOutGpio != SMU7_UNUSED_GPIO_PIN) {
		fTable.ThermOutGpio = fThermOutGpio;
		// the VBIOS programs the inactive state
		fTable.ThermOutPolarity = (ReadReg4AmdGpu(mmGPIOPAD_A)
			& (1 << fThermOutGpio)) == 0 ? 1 : 0;
		fTable.ThermOutMode = SMU7_THERM_OUT_MODE_THERM_ONLY;
	} else {
		fTable.ThermOutGpio = 17;
		fTable.ThermOutPolarity = 1;
		fTable.ThermOutMode = SMU7_THERM_OUT_MODE_DISABLE;
	}

	fTableBuilt = true;
	return B_OK;
}


void
PolarisDpm::PrintTable()
{
	if (!fTableBuilt)
		return;
	printf("DPM table: %u engine, %u memory, %u link levels; boot levels %u /"
		" %u / %u\n", fTable.GraphicsDpmLevelCount, fTable.MemoryDpmLevelCount,
		fTable.LinkLevelCount, fTable.GraphicsBootLevel,
		fTable.MemoryBootLevel, fTable.PCIeBootLinkLevel);
	for (uint32 i = 0; i < fTable.GraphicsDpmLevelCount; i++) {
		const SMU74_Discrete_GraphicsLevel &level = fTable.GraphicsLevel[i];
		const SMU_SclkSetting &setting = level.SclkSetting;
		uint32 voltage = HostUl(level.MinVoltage);
		printf("  sclk %" B_PRIu32 ": %4" B_PRIu32 " MHz, vddc %4" B_PRIu32
			" mV, vddci %4" B_PRIu32 " mV, fcw %u + %#06x, pcc %u, range %u,"
			" ss %u (%u + %#06x), pcie %u\n", i,
			HostUl(setting.SclkFrequency) / 100,
			(voltage & 0x7fff) / VOLTAGE_SCALE,
			((voltage >> VDDCI_SHIFT) & 0x7fff) / VOLTAGE_SCALE,
			HostUs(setting.Fcw_int), HostUs(setting.Fcw_frac),
			HostUs(setting.Pcc_fcw_int), setting.PllRange, setting.SSc_En,
			HostUs(setting.Fcw1_int), HostUs(setting.Fcw1_frac),
			level.pcieDpmLevel);
	}
	for (uint32 i = 0; i < fTable.MemoryDpmLevelCount; i++) {
		const SMU74_Discrete_MemoryLevel &level = fTable.MemoryLevel[i];
		uint32 voltage = HostUl(level.MinVoltage);
		printf("  mclk %" B_PRIu32 ": %4" B_PRIu32 " MHz, vddc %4" B_PRIu32
			" mV, mvdd %4" B_PRIu32 " mV, watermark %u\n", i,
			HostUl(level.MclkFrequency) / 100,
			(voltage & 0x7fff) / VOLTAGE_SCALE,
			HostUl(level.MinMvdd) / VOLTAGE_SCALE, level.DisplayWatermark);
	}
	for (uint32 i = 0; i <= fTable.LinkLevelCount; i++) {
		const SMU74_Discrete_LinkLevel &level = fTable.LinkLevel[i];
		printf("  link %" B_PRIu32 ": gen %u, lanes code %u, bif dfs %u\n", i,
			level.PcieGenSpeed + 1, level.PcieLaneCount,
			HostUs(level.BifSclkDfs));
	}
	printf("  acpi: sclk %" B_PRIu32 " MHz, mclk %" B_PRIu32 " MHz; ulv bif"
		" dfs %u\n", HostUl(fTable.ACPILevel.SclkSetting.SclkFrequency) / 100,
		HostUl(fTable.MemoryACPILevel.MclkFrequency) / 100,
		HostUs(fTable.Ulv.BifSclkDfs));
	for (uint32 i = 0; i < fTable.UvdLevelCount; i++) {
		printf("  mm %" B_PRIu32 ": vclk div %u, dclk div %u, eclk div %u,"
			" samclk div %u\n", i, fTable.UvdLevel[i].VclkDivider,
			fTable.UvdLevel[i].DclkDivider, fTable.VceLevel[i].Divider,
			fTable.SamuLevel[i].Divider);
	}
	printf("  VRConfig %#" B_PRIx32 ", flags %#" B_PRIx32 ", temperature limit"
		" %u C, VR hot GPIO %u, thermal out GPIO %u mode %u\n",
		HostUl(fTable.VRConfig), HostUl(fTable.SystemFlags),
		HostUs(fTable.TemperatureLimitHigh) / SMU7_Q88_FORMAT_CONVERSION_UNIT,
		fTable.VRHotGpio, fTable.ThermOutGpio, fTable.ThermOutMode);
}


// #pragma mark - upload and start


// smu7_copy_and_switch_arb_sets()
void
PolarisDpm::SwitchArbSet(uint32 source, uint32 destination)
{
	uint32 timing, timing2, burstTime;
	uint32 burst = ReadReg4AmdGpu(mmMC_ARB_BURST_TIME);
	if (source == MC_CG_ARB_FREQ_F0) {
		timing = ReadReg4AmdGpu(mmMC_ARB_DRAM_TIMING);
		timing2 = ReadReg4AmdGpu(mmMC_ARB_DRAM_TIMING2);
		burstTime = (burst & MC_ARB_BURST_TIME__STATE0_MASK)
			>> MC_ARB_BURST_TIME__STATE0__SHIFT;
	} else {
		timing = ReadReg4AmdGpu(mmMC_ARB_DRAM_TIMING_1);
		timing2 = ReadReg4AmdGpu(mmMC_ARB_DRAM_TIMING2_1);
		burstTime = (burst & MC_ARB_BURST_TIME__STATE1_MASK)
			>> MC_ARB_BURST_TIME__STATE1__SHIFT;
	}
	if (destination == MC_CG_ARB_FREQ_F0) {
		WriteReg4AmdGpu(mmMC_ARB_DRAM_TIMING, timing);
		WriteReg4AmdGpu(mmMC_ARB_DRAM_TIMING2, timing2);
		WriteReg4AmdGpu(mmMC_ARB_BURST_TIME, (burst
			& ~MC_ARB_BURST_TIME__STATE0_MASK)
			| burstTime << MC_ARB_BURST_TIME__STATE0__SHIFT);
	} else {
		WriteReg4AmdGpu(mmMC_ARB_DRAM_TIMING_1, timing);
		WriteReg4AmdGpu(mmMC_ARB_DRAM_TIMING2_1, timing2);
		WriteReg4AmdGpu(mmMC_ARB_BURST_TIME, (burst
			& ~MC_ARB_BURST_TIME__STATE1_MASK)
			| burstTime << MC_ARB_BURST_TIME__STATE1__SHIFT);
	}
	WriteReg4AmdGpu(mmMC_CG_CONFIG, ReadReg4AmdGpu(mmMC_CG_CONFIG) | 0xf);
	WriteReg4AmdGpu(mmMC_ARB_CG, (ReadReg4AmdGpu(mmMC_ARB_CG)
		& ~MC_ARB_CG__CG_ARB_REQ_MASK)
		| destination << MC_ARB_CG__CG_ARB_REQ__SHIFT);
}


// polaris10_program_memory_timing_parameters(): the VBIOS computes the
// arbiter timings of each clock pair into the F0 set, which isn't the
// active one after SwitchArbSet(F0, F1)
status_t
PolarisDpm::BuildArbTable()
{
	memset(&fArbTable, 0, sizeof(fArbTable));
	for (uint32 i = 0; i < fSclkCount; i++) {
		for (uint32 j = 0; j < fMclkCount; j++) {
			CheckRet(SetEngineDramTimings(fPowerPlay.Sclk(i).clock,
				fPowerPlay.Mclk(j).clock));
			SMU74_Discrete_MCArbDramTimingTableEntry &entry
				= fArbTable.entries[i][j];
			entry.McArbDramTiming
				= SmcUl(ReadReg4AmdGpu(mmMC_ARB_DRAM_TIMING));
			entry.McArbDramTiming2
				= SmcUl(ReadReg4AmdGpu(mmMC_ARB_DRAM_TIMING2));
			entry.McArbBurstTime = (ReadReg4AmdGpu(mmMC_ARB_BURST_TIME)
				& MC_ARB_BURST_TIME__STATE0_MASK)
				>> MC_ARB_BURST_TIME__STATE0__SHIFT;
			if (i == 0)
				CheckRet(SetAcTiming(fPowerPlay.Mclk(j).clock, j));
		}
	}
	return B_OK;
}


status_t
PolarisDpm::Upload()
{
	if (!fTableBuilt)
		return B_NOT_INITIALIZED;
	if (IsRunning()) {
		printf("[!] DPM: already running\n");
		return B_BUSY;
	}

	// smu7_enable_dpm_tasks() up to smum_init_smc_table()

	// smu7_program_static_screen_threshold_parameters()
	uint32 value = fSmu.ReadIndirect(ixCG_STATIC_SCREEN_PARAMETER);
	value &= ~(CG_STATIC_SCREEN_PARAMETER__STATIC_SCREEN_THRESHOLD_UNIT_MASK
		| CG_STATIC_SCREEN_PARAMETER__STATIC_SCREEN_THRESHOLD_MASK);
	value |= SMU7_STATICSCREENTHRESHOLDUNIT_DFLT
			<< CG_STATIC_SCREEN_PARAMETER__STATIC_SCREEN_THRESHOLD_UNIT__SHIFT
		| SMU7_STATICSCREENTHRESHOLD_DFLT
			<< CG_STATIC_SCREEN_PARAMETER__STATIC_SCREEN_THRESHOLD__SHIFT;
	fSmu.WriteIndirect(ixCG_STATIC_SCREEN_PARAMETER, value);

	// smu7_enable_display_gap()
	value = fSmu.ReadIndirect(ixCG_DISPLAY_GAP_CNTL);
	value &= ~(CG_DISPLAY_GAP_CNTL__DISP_GAP_MASK
		| CG_DISPLAY_GAP_CNTL__DISP_GAP_MCHG_MASK);
	value |= DISPLAY_GAP_IGNORE << CG_DISPLAY_GAP_CNTL__DISP_GAP__SHIFT
		| DISPLAY_GAP_VBLANK << CG_DISPLAY_GAP_CNTL__DISP_GAP_MCHG__SHIFT;
	fSmu.WriteIndirect(ixCG_DISPLAY_GAP_CNTL, value);

	// smu7_program_voting_clients()
	fSmu.WriteIndirect(ixSCLK_PWRMGT_CNTL, fSmu.ReadIndirect(ixSCLK_PWRMGT_CNTL)
		& ~(SCLK_PWRMGT_CNTL__RESET_SCLK_CNT_MASK
			| SCLK_PWRMGT_CNTL__RESET_BUSY_CNT_MASK));
	for (uint32 i = 0; i < 8; i++)
		fSmu.WriteIndirect(ixCG_FREQ_TRAN_VOTING_0 + i * 4, kVotingRights[i]);

	// smu7_initial_switch_from_arbf0_to_f1(), unless an earlier upload did
	// (F0 then holds the timings of the last clock pair)
	uint32 arbSet = (ReadReg4AmdGpu(mmMC_ARB_CG) & MC_ARB_CG__CG_ARB_REQ_MASK)
		>> MC_ARB_CG__CG_ARB_REQ__SHIFT;
	printf("DPM: MC_ARB_CG %#" B_PRIx32 ", SMC_SCRATCH9 %#" B_PRIx32 "\n",
		ReadReg4AmdGpu(mmMC_ARB_CG), fSmu.ReadIndirect(ixSMC_SCRATCH9));
	// no request since boot (0) means F0
	if (arbSet == 0 || arbSet == MC_CG_ARB_FREQ_F0)
		SwitchArbSet(MC_CG_ARB_FREQ_F0, MC_CG_ARB_FREQ_F1);
	else if (arbSet != MC_CG_ARB_FREQ_F1) {
		printf("[!] DPM: unexpected MC arbiter set %#" B_PRIx32 "\n", arbSet);
		return B_ERROR;
	}

	CheckRet(BuildArbTable());
	CheckRet(CopyToSmc(fArbTableStart, &fArbTable, sizeof(fArbTable)));

	// everything after the PID controllers, which the SMC firmware sets up
	size_t offset = offsetof(SMU74_Discrete_DpmTable, SystemFlags);
	CheckRet(CopyToSmc(fDpmTableStart + offset, (uint8*)&fTable + offset,
		sizeof(fTable) - offset));
	return B_OK;
}


bool
PolarisDpm::IsRunning()
{
	// polaris10_is_dpm_running()
	return (fSmu.ReadIndirect(ixFEATURE_STATUS)
		& FEATURE_STATUS__VOLTAGE_CONTROLLER_ON_MASK) != 0;
}


status_t
PolarisDpm::Start(bool memoryDpm)
{
	// smu7_enable_voltage_control()
	fSmu.WriteIndirect(ixGENERAL_PWRMGT, fSmu.ReadIndirect(ixGENERAL_PWRMGT)
		| GENERAL_PWRMGT__VOLT_PWRMGT_EN_MASK);

	// smu7_notify_has_display(), smu7_enable_sclk_control()
	CheckRet(fSmu.SendMessage(PPSMC_HasDisplay, 0));
	fSmu.WriteIndirect(ixSCLK_PWRMGT_CNTL, fSmu.ReadIndirect(ixSCLK_PWRMGT_CNTL)
		& ~SCLK_PWRMGT_CNTL__SCLK_PWRMGT_OFF_MASK);

	// smu7_enable_smc_voltage_controller()
	fSmu.WriteIndirect(ixPWR_SVI2_PLANE1_LOAD,
		fSmu.ReadIndirect(ixPWR_SVI2_PLANE1_LOAD)
			& ~(PWR_SVI2_PLANE1_LOAD__PSI1_MASK
				| PWR_SVI2_PLANE1_LOAD__PSI0_EN_MASK));
	CheckRet(fSmu.SendMessage(PPSMC_MSG_Voltage_Cntl_Enable, 0));

	CheckRet(fSmu.SendMessage(PPSMC_MSG_MASTER_DeepSleep_OFF, 0));

	// smu7_start_dpm()
	fSmu.WriteIndirect(ixGENERAL_PWRMGT, fSmu.ReadIndirect(ixGENERAL_PWRMGT)
		| GENERAL_PWRMGT__GLOBAL_PWRMGT_EN_MASK);
	fSmu.WriteIndirect(ixSCLK_PWRMGT_CNTL, fSmu.ReadIndirect(ixSCLK_PWRMGT_CNTL)
		| SCLK_PWRMGT_CNTL__DYNAMIC_PM_EN_MASK);
	fSmu.WriteIndirect(fSoftRegsStart
		+ offsetof(SMU74_SoftRegisters, VoltageChangeTimeout), 0x1000);

	// smu7_enable_sclk_mclk_dpm()
	uint32 handshake = fSoftRegsStart
		+ offsetof(SMU74_SoftRegisters, HandshakeDisables);
	fSmu.WriteIndirect(handshake, fSmu.ReadIndirect(handshake)
		| SMU7_VCE_SCLK_HANDSHAKE_DISABLE);
	CheckRet(fSmu.SendMessage(PPSMC_MSG_DPM_Enable, 0));
	if (memoryDpm)
		CheckRet(EnableMemoryDpm());
	CheckRet(fSmu.SendMessage(PPSMC_MSG_PCIeDPM_Disable, 0));

	// smu7_enable_thermal_auto_throttle()
	fSmu.WriteIndirect(ixCG_THERMAL_CTRL, (fSmu.ReadIndirect(ixCG_THERMAL_CTRL)
			& ~CG_THERMAL_CTRL__DPM_EVENT_SRC_MASK)
		| DPM_EVENT_SRC_DIGITAL << CG_THERMAL_CTRL__DPM_EVENT_SRC__SHIFT);
	fSmu.WriteIndirect(ixGENERAL_PWRMGT, fSmu.ReadIndirect(ixGENERAL_PWRMGT)
		& ~GENERAL_PWRMGT__THERMAL_PROTECTION_DIS_MASK);

	// smu7_upload_dpm_level_enable_mask()
	CheckRet(fSmu.SendMessage(PPSMC_MSG_SCLKDPM_SetEnabledMask,
		fSclkEnableMask));
	if (memoryDpm) {
		CheckRet(fSmu.SendMessage(PPSMC_MSG_MCLKDPM_SetEnabledMask,
			fMclkEnableMask));
	}
	return B_OK;
}


// the memory part of smu7_enable_sclk_mclk_dpm()
status_t
PolarisDpm::EnableMemoryDpm()
{
	ProgramDisplayGap();
	CheckRet(fSmu.SendMessage(PPSMC_MSG_MCLKDPM_Enable, 0));
	WriteReg4AmdGpu(mmMC_SEQ_CNTL_3, ReadReg4AmdGpu(mmMC_SEQ_CNTL_3)
		| MC_SEQ_CNTL_3__CAC_EN_MASK);
	fSmu.WriteIndirect(ixLCAC_MC0_CNTL, 0x5);
	fSmu.WriteIndirect(ixLCAC_MC1_CNTL, 0x5);
	fSmu.WriteIndirect(ixLCAC_CPL_CNTL, 0x100005);
	snooze(10);
	fSmu.WriteIndirect(ixLCAC_MC0_CNTL, 0x400005);
	fSmu.WriteIndirect(ixLCAC_MC1_CNTL, 0x400005);
	fSmu.WriteIndirect(ixLCAC_CPL_CNTL, 0x500005);
	return B_OK;
}


// memory clock DPM after Start(false); the table and arbiter timings in SMC
// RAM are still those of Upload()
status_t
PolarisDpm::StartMemory()
{
	if (!IsRunning()) {
		printf("[!] DPM: not running\n");
		return B_NOT_INITIALIZED;
	}
	CheckRet(EnableMemoryDpm());
	return fSmu.SendMessage(PPSMC_MSG_MCLKDPM_SetEnabledMask,
		fMclkEnableMask);
}


// smu7_program_display_gap() for one 60 Hz display
void
PolarisDpm::ProgramDisplayGap()
{
	const uint32 frameTime = 1000000 / 60;		// us
	const uint32 minVblankTime = 0;				// us
	uint32 preVblankTime = frameTime - 200 - minVblankTime;

	uint32 value = fSmu.ReadIndirect(ixCG_DISPLAY_GAP_CNTL);
	value &= ~CG_DISPLAY_GAP_CNTL__DISP_GAP_MASK;
	value |= DISPLAY_GAP_VBLANK_OR_WM << CG_DISPLAY_GAP_CNTL__DISP_GAP__SHIFT;
	fSmu.WriteIndirect(ixCG_DISPLAY_GAP_CNTL, value);

	// in reference clock ticks
	fSmu.WriteIndirect(ixCG_DISPLAY_GAP_CNTL2, preVblankTime * (fXclk / 100));
	fSmu.WriteIndirect(fSoftRegsStart
		+ offsetof(SMU74_SoftRegisters, PreVBlankGap), 0x64);
	fSmu.WriteIndirect(fSoftRegsStart
		+ offsetof(SMU74_SoftRegisters, VBlankTimeout),
		frameTime - preVblankTime);
}


// polaris10_populate_and_upload_sclk_mclk_dpm_levels() with
// smu7_freeze_sclk_mclk_dpm(): the engine clock levels are replaced while
// DPM runs
status_t
PolarisDpm::ReloadGraphicsLevels()
{
	if (!fTableBuilt)
		return B_NOT_INITIALIZED;
	if (!IsRunning())
		return B_NOT_INITIALIZED;
	CheckRet(fSmu.SendMessage(PPSMC_MSG_SCLKDPM_FreezeLevel, 0));
	status_t status = CopyToSmc(fDpmTableStart
		+ offsetof(SMU74_Discrete_DpmTable, GraphicsLevel),
		fTable.GraphicsLevel, sizeof(fTable.GraphicsLevel));
	status_t unfreezeStatus
		= fSmu.SendMessage(PPSMC_MSG_SCLKDPM_UnfreezeLevel, 0);
	return status < B_OK ? status : unfreezeStatus;
}
