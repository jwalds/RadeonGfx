#include "PolarisPowerPlay.h"

#include <stdio.h>
#include <stddef.h>

#include "vi/pptable_v1_0.h"
#include "atombios/atom.h"


enum {
	kVirtualVoltageId0 = 0xff01,
	kVirtualVoltageCount = 8,
	kMaxSafeVoltage = 2000,		// mV
};


template<typename Type>
static const Type *
At(const uint8 *base, size_t size, size_t offset)
{
	if (offset == 0 || offset + sizeof(Type) > size)
		return NULL;
	return (const Type*)(base + offset);
}


status_t
PolarisPowerPlay::Init(const uint8 *rom, size_t size)
{
	const uint16 *romHeaderOffset = At<uint16>(rom, size,
		OFFSET_TO_POINTER_TO_ATOM_ROM_HEADER);
	if (romHeaderOffset == NULL)
		return B_BAD_DATA;
	const ATOM_ROM_HEADER *romHeader = At<ATOM_ROM_HEADER>(rom, size,
		*romHeaderOffset);
	if (romHeader == NULL)
		return B_BAD_DATA;
	const ATOM_MASTER_DATA_TABLE *master = At<ATOM_MASTER_DATA_TABLE>(rom,
		size, romHeader->usMasterDataTableOffset);
	if (master == NULL)
		return B_BAD_DATA;
	size_t tableOffset = master->ListOfDataTables.PowerPlayInfo;
	const ATOM_Tonga_POWERPLAYTABLE *table
		= At<ATOM_Tonga_POWERPLAYTABLE>(rom, size, tableOffset);
	if (table == NULL)
		return B_BAD_DATA;
	const uint8 *base = rom + tableOffset;
	size_t tableSize = size - tableOffset;

	fTableRevision = table->ucTableRevision;
	fPlatformCaps = table->ulPlatformCaps;
	fMaxOdSclk = table->ulMaxODEngineClock;
	fMaxOdMclk = table->ulMaxODMemoryClock;
	fPowerLimit = table->usPowerControlLimit;
	if (table->sHeader.ucTableFormatRevision < 7)
		return B_NOT_SUPPORTED;

	const ATOM_Tonga_PowerTune_Table *powerTune
		= At<ATOM_Tonga_PowerTune_Table>(base, tableSize,
			table->usPowerTuneTableOffset);
	if (powerTune != NULL)
		fClockStretchAmount = powerTune->usClockStretchAmount;

	const ATOM_Tonga_Voltage_Lookup_Table *vddc
		= At<ATOM_Tonga_Voltage_Lookup_Table>(base, tableSize,
			table->usVddcLookupTableOffset);
	if (vddc == NULL)
		return B_BAD_DATA;
	fVddcCount = vddc->ucNumEntries < 32 ? vddc->ucNumEntries : 32;
	for (uint32 i = 0; i < fVddcCount; i++)
		fVddc[i] = vddc->entries[i].usVdd;

	const ATOM_Tonga_SCLK_Dependency_Table *sclk
		= At<ATOM_Tonga_SCLK_Dependency_Table>(base, tableSize,
			table->usSclkDependencyTableOffset);
	if (sclk == NULL)
		return B_BAD_DATA;
	fSclkCount = sclk->ucNumEntries < kMaxLevels ? sclk->ucNumEntries
		: kMaxLevels;
	for (uint32 i = 0; i < fSclkCount; i++) {
		uint32 clock;
		uint8 vddInd, cksv;
		if (sclk->ucRevId == 0) {
			const ATOM_Tonga_SCLK_Dependency_Record &record = sclk->entries[i];
			clock = record.ulSclk;
			vddInd = record.ucVddInd;
			cksv = record.ucCKSVOffsetandDisable;
		} else {
			const ATOM_Polaris_SCLK_Dependency_Record &record
				= ((const ATOM_Polaris_SCLK_Dependency_Table*)sclk)->entries[i];
			clock = record.ulSclk;
			vddInd = record.ucVddInd;
			cksv = record.ucCKSVOffsetandDisable;
		}
		fSclk[i].clock = clock;
		fSclk[i].vddcIndex = vddInd;
		fSclk[i].cksEnable = (cksv & 0x80) == 0;
	}

	const ATOM_Tonga_MCLK_Dependency_Table *mclk
		= At<ATOM_Tonga_MCLK_Dependency_Table>(base, tableSize,
			table->usMclkDependencyTableOffset);
	if (mclk == NULL)
		return B_BAD_DATA;
	fMclkCount = mclk->ucNumEntries < kMaxLevels ? mclk->ucNumEntries
		: kMaxLevels;
	for (uint32 i = 0; i < fMclkCount; i++) {
		const ATOM_Tonga_MCLK_Dependency_Record &record = mclk->entries[i];
		fMclk[i].clock = record.ulMclk;
		fMclk[i].vddcIndex = record.ucVddcInd;
		fMclk[i].vddci = record.usVddci;
		fMclk[i].mvdd = record.usMvdd;
	}
	UpdateLevelVoltages();
	return B_OK;
}


// Replaces the virtual voltage IDs (0xff01..0xff08) of the VDDC lookup table
// with the voltages the VBIOS computes for this chip's leakage (EVV, Linux
// smu7_get_evv_voltages). Only runs ATOM GetVoltageInfo; changes no voltage.
status_t
PolarisPowerPlay::ResolveVoltages(atom_context *atom)
{
	for (uint32 id = 0; id < kVirtualVoltageCount; id++) {
		uint16 virtualId = kVirtualVoltageId0 + id;

		// the lowest sclk level that uses this voltage
		uint32 level = 0;
		for (; level < fSclkCount; level++) {
			if (fSclk[level].vddcIndex < fVddcCount
				&& fVddc[fSclk[level].vddcIndex] == virtualId)
				break;
		}
		if (level >= fSclkCount)
			continue;
		uint32 sclk = fSclk[level].clock;
		if (fClockStretchAmount != 0) {
			for (uint32 i = 1; i < fSclkCount; i++) {
				if (fSclk[i].clock == sclk && !fSclk[i].cksEnable) {
					sclk += 5000;
					break;
				}
			}
		}

		union {
			GET_VOLTAGE_INFO_INPUT_PARAMETER_V1_3 in;
			GET_EVV_VOLTAGE_INFO_OUTPUT_PARAMETER_V1_3 out;
		} args = {};
		args.in.ucVoltageType = VOLTAGE_TYPE_VDDC;
		args.in.ucVoltageMode = ATOM_GET_VOLTAGE_EVV_VOLTAGE;
		args.in.usVoltageLevel = virtualId;
		args.in.ulSCLKFreq = sclk;
		status_t status = atom_execute_table(atom,
			GetIndexIntoMasterTable(COMMAND, GetVoltageInfo), (uint32*)&args);
		if (status < B_OK)
			return status;

		// in 0.01 mV
		uint32 voltage = args.out.ulVoltageLevel / 100;
		if (voltage == 0 || voltage >= kMaxSafeVoltage) {
			printf("[!] PowerPlay: EVV voltage %" B_PRIu32 " mV for %#x at %"
				B_PRIu32 " MHz is out of range\n", voltage, virtualId,
				sclk / 100);
			return B_BAD_DATA;
		}
		for (uint32 i = 0; i < fVddcCount; i++) {
			if (fVddc[i] == virtualId)
				fVddc[i] = voltage;
		}
	}
	UpdateLevelVoltages();
	return B_OK;
}


void
PolarisPowerPlay::UpdateLevelVoltages()
{
	for (uint32 i = 0; i < fSclkCount; i++) {
		fSclk[i].vddc = fSclk[i].vddcIndex < fVddcCount
			? fVddc[fSclk[i].vddcIndex] : 0;
	}
	for (uint32 i = 0; i < fMclkCount; i++) {
		fMclk[i].vddc = fMclk[i].vddcIndex < fVddcCount
			? fVddc[fMclk[i].vddcIndex] : 0;
	}
}


static void
PrintVoltage(uint16 voltage)
{
	if (voltage >= kVirtualVoltageId0)
		printf("%#x", voltage);
	else
		printf("%4u mV", voltage);
}


void
PolarisPowerPlay::Print()
{
	printf("PowerPlay: table revision %u, caps %#" B_PRIx32 ", overdrive"
		" limits %" B_PRIu32 " / %" B_PRIu32 " MHz, power limit %u W, clock"
		" stretch %u\n", fTableRevision, fPlatformCaps, fMaxOdSclk / 100,
		fMaxOdMclk / 100, fPowerLimit, fClockStretchAmount);
	for (uint32 i = 0; i < fSclkCount; i++) {
		printf("  sclk %" B_PRIu32 ": %4" B_PRIu32 " MHz, ", i,
			fSclk[i].clock / 100);
		PrintVoltage(fSclk[i].vddc);
		printf("%s\n", fSclk[i].cksEnable ? "" : ", no clock stretching");
	}
	for (uint32 i = 0; i < fMclkCount; i++) {
		printf("  mclk %" B_PRIu32 ": %4" B_PRIu32 " MHz, vddc ", i,
			fMclk[i].clock / 100);
		PrintVoltage(fMclk[i].vddc);
		printf(", vddci %4u mV, mvdd %4u mV\n", fMclk[i].vddci,
			fMclk[i].mvdd);
	}
}
