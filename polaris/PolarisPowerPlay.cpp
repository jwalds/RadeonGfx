#include "PolarisPowerPlay.h"

#include <stdio.h>
#include <stddef.h>

#include "vi/pptable_v1_0.h"


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
	printf("PowerPlay: ROM header at %#x, master data table at %#x, PowerPlay"
		" table at %#zx: format %u.%u, size %u\n", *romHeaderOffset,
		romHeader->usMasterDataTableOffset, tableOffset,
		table->sHeader.ucTableFormatRevision,
		table->sHeader.ucTableContentRevision,
		table->sHeader.usStructureSize);
	for (uint32 i = 0; i < 96; i++)
		printf("%02x%s", rom[tableOffset + i], i % 32 == 31 ? "\n" : " ");
	printf("sizeof(ATOM_Tonga_POWERPLAYTABLE) %zu, offsetof caps %zu\n",
		sizeof(ATOM_Tonga_POWERPLAYTABLE),
		offsetof(ATOM_Tonga_POWERPLAYTABLE, ulPlatformCaps));
	const uint8 *base = rom + tableOffset;
	size_t tableSize = size - tableOffset;

	fTableRevision = table->ucTableRevision;
	fPlatformCaps = table->ulPlatformCaps;
	fMaxOdSclk = table->ulMaxODEngineClock;
	fMaxOdMclk = table->ulMaxODMemoryClock;
	fPowerLimit = table->usPowerControlLimit;
	if (table->sHeader.ucTableFormatRevision < 7)
		return B_NOT_SUPPORTED;

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
		fSclk[i].vddc = vddInd < fVddcCount ? fVddc[vddInd] : 0;
		fSclk[i].enabled = (cksv & 0x80) == 0;
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
		fMclk[i].vddc = record.ucVddcInd < fVddcCount
			? fVddc[record.ucVddcInd] : 0;
		fMclk[i].vddci = record.usVddci;
		fMclk[i].mvdd = record.usMvdd;
	}
	return B_OK;
}


void
PolarisPowerPlay::Print()
{
	printf("PowerPlay: table revision %u, caps %#" B_PRIx32 ", overdrive"
		" limits %" B_PRIu32 " / %" B_PRIu32 " MHz, power limit %u W\n",
		fTableRevision, fPlatformCaps, fMaxOdSclk / 100, fMaxOdMclk / 100,
		fPowerLimit);
	for (uint32 i = 0; i < fSclkCount; i++) {
		printf("  sclk %" B_PRIu32 ": %4" B_PRIu32 " MHz, %4u mV%s\n", i,
			fSclk[i].clock / 100, fSclk[i].vddc,
			fSclk[i].enabled ? "" : " (disabled)");
	}
	for (uint32 i = 0; i < fMclkCount; i++) {
		printf("  mclk %" B_PRIu32 ": %4" B_PRIu32 " MHz, vddc %4u mV, vddci %4u"
			" mV, mvdd %4u mV\n", i, fMclk[i].clock / 100, fMclk[i].vddc,
			fMclk[i].vddci, fMclk[i].mvdd);
	}
}
