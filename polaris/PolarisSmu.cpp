#include "PolarisSmu.h"
#include "RadeonDevice.h"
#include "Radeon.h"

#include <stdio.h>
#include <string.h>
#include <File.h>
#include <OS.h>
#include <String.h>

#define CheckRet(err) {status_t _err = (err); if (_err < B_OK) return _err;}

// SMC indirect access (Linux smu_7_1_1_d.h, smu_7_1_3_d.h)
#define mmSMC_IND_INDEX_11			0x1ac
#define mmSMC_IND_DATA_11			0x1ad
#define mmSMC_IND_ACCESS_CNTL		0x92
#define SMC_IND_ACCESS_CNTL__AUTO_INCREMENT_IND_11_MASK	0x800
#define mmSMC_MESSAGE_0				0x94
#define mmSMC_RESP_0				0x95
#define mmSMC_MSG_ARG_0				0xa4

#define ixSMC_SYSCON_RESET_CNTL		0x80000000
#define ixSMC_SYSCON_CLOCK_CNTL_0	0x80000004
#define ixSMC_PC_C					0x80000370
#define ixRCU_UC_EVENTS				0xc0000004
#define ixCG_MULT_THERMAL_STATUS	0xc0300014
#define ixSMU_STATUS				0xe0003088
#define ixSMU_FIRMWARE				0xe00030a4
#define ixFIRMWARE_FLAGS			0x3f000

#define SMC_SYSCON_RESET_CNTL__rst_reg_MASK				0x1
#define SMC_SYSCON_CLOCK_CNTL_0__ck_disable_MASK		0x1
#define RCU_UC_EVENTS__boot_seq_done_MASK				0x80
#define RCU_UC_EVENTS__INTERRUPTS_ENABLED_MASK			0x10000
#define SMU_STATUS__SMU_DONE_MASK						0x1
#define SMU_STATUS__SMU_PASS_MASK						0x2
#define SMU_FIRMWARE__SMU_MODE_MASK						0x10000
#define SMU_FIRMWARE__SMU_SEL_MASK						0x20000
#define FIRMWARE_FLAGS__INTERRUPTS_ENABLED_MASK			0x1
#define CG_MULT_THERMAL_STATUS__CTF_TEMP_MASK			0x3fe00
#define CG_MULT_THERMAL_STATUS__CTF_TEMP__SHIFT			9

// PPSMC messages (Linux smu7_ppsmc.h)
#define PPSMC_MSG_Test					0x100
#define PPSMC_MSG_DRV_DRAM_ADDR_HI		0x250
#define PPSMC_MSG_DRV_DRAM_ADDR_LO		0x251
#define PPSMC_MSG_SMU_DRAM_ADDR_HI		0x252
#define PPSMC_MSG_SMU_DRAM_ADDR_LO		0x253
#define PPSMC_MSG_LoadUcodes			0x254

// SMC SRAM layout (Linux smu74.h)
#define SMU7_SMC_SIZE					0x20000
#define SMU7_FIRMWARE_HEADER_LOCATION	0x20000
#define SMU74_FIRMWARE_HEADER_SOFT_REGISTERS	48	// offsetof(SoftRegisters)
#define SMU74_SOFT_REGISTERS_UCODE_LOAD_STATUS	108	// offsetof(UcodeLoadStatus)

static const bigtime_t kTimeout = 1000000;


struct common_firmware_header {
	uint32 size_bytes;
	uint32 header_size_bytes;
	uint16 header_version_major;
	uint16 header_version_minor;
	uint16 ip_version_major;
	uint16 ip_version_minor;
	uint32 ucode_version;
	uint32 ucode_size_bytes;
	uint32 ucode_array_offset_bytes;
	uint32 crc32;
};

// Linux smu_ucode_xfer_vi.h
struct SMU_Entry {
	uint16 id;
	uint16 version;
	uint32 image_addr_high;
	uint32 image_addr_low;
	uint32 meta_data_addr_high;
	uint32 meta_data_addr_low;
	uint32 data_size_byte;
	uint16 flags;
	uint16 num_register_entries;
};

#define SMU_MAX_ENTRIES 12

struct SMU_DRAMData_TOC {
	uint32 structure_version;
	uint32 num_entries;
	SMU_Entry entry[SMU_MAX_ENTRIES];
};


static status_t
ReadFile(const char *path, ArrayDeleter<uint8> &data, off_t &size)
{
	BFile file(path, B_READ_ONLY);
	CheckRet(file.InitCheck());
	CheckRet(file.GetSize(&size));
	if (size < (off_t)sizeof(common_firmware_header) || size > 4 * 1024 * 1024)
		return B_BAD_DATA;
	data.SetTo(new(std::nothrow) uint8[size]);
	if (!data.IsSet())
		return B_NO_MEMORY;
	if (file.ReadAt(0, data.Get(), size) != size)
		return B_IO_ERROR;
	const common_firmware_header &header
		= *(const common_firmware_header*)data.Get();
	if (header.size_bytes != size
		|| header.ucode_array_offset_bytes + header.ucode_size_bytes > size
		|| header.ucode_size_bytes % 4 != 0)
		return B_BAD_DATA;
	return B_OK;
}


PolarisSmu::PolarisSmu()
{
}


PolarisSmu::~PolarisSmu()
{
}


uint32
PolarisSmu::ReadIndirect(uint32 address)
{
	WriteReg4AmdGpu(mmSMC_IND_INDEX_11, address);
	return ReadReg4AmdGpu(mmSMC_IND_DATA_11);
}


void
PolarisSmu::WriteIndirect(uint32 address, uint32 value)
{
	WriteReg4AmdGpu(mmSMC_IND_INDEX_11, address);
	WriteReg4AmdGpu(mmSMC_IND_DATA_11, value);
}


status_t
PolarisSmu::WaitIndirect(uint32 address, uint32 mask, uint32 value,
	bool equal, const char *what)
{
	bigtime_t start = system_time();
	uint32 current;
	for (;;) {
		current = ReadIndirect(address);
		if (((current & mask) == value) == equal)
			return B_OK;
		if (system_time() - start > kTimeout)
			break;
		snooze(10);
	}
	printf("  [!] timeout waiting for %s (%#" B_PRIx32 " = %#010" B_PRIx32
		")\n", what, address, current);
	return B_TIMED_OUT;
}


bool
PolarisSmu::IsFirmwareRunning()
{
	// smu7_is_smc_ram_running()
	return (ReadIndirect(ixSMC_SYSCON_CLOCK_CNTL_0)
			& SMC_SYSCON_CLOCK_CNTL_0__ck_disable_MASK) == 0
		&& ReadIndirect(ixSMC_PC_C) >= 0x20100;
}


status_t
PolarisSmu::SendMessage(uint16 message, uint32 parameter, uint32 *result)
{
	// smu7_send_msg_to_smc_with_parameter()
	bigtime_t start = system_time();
	while (ReadReg4AmdGpu(mmSMC_RESP_0) == 0) {
		if (system_time() - start > kTimeout) {
			printf("  [!] SMU busy before message %#x\n", message);
			return B_TIMED_OUT;
		}
		snooze(10);
	}
	WriteReg4AmdGpu(mmSMC_MSG_ARG_0, parameter);
	WriteReg4AmdGpu(mmSMC_RESP_0, 0);
	WriteReg4AmdGpu(mmSMC_MESSAGE_0, message);

	start = system_time();
	uint32 response;
	while ((response = ReadReg4AmdGpu(mmSMC_RESP_0)) == 0) {
		if (system_time() - start > kTimeout) {
			printf("  [!] no SMU response to message %#x\n", message);
			return B_TIMED_OUT;
		}
		snooze(10);
	}
	if (result != NULL)
		*result = ReadReg4AmdGpu(mmSMC_MSG_ARG_0);
	if (response != 1) {
		printf("  [!] SMU message %#x (%#" B_PRIx32 ") failed: response %#"
			B_PRIx32 "\n", message, parameter, response);
		return B_ERROR;
	}
	return B_OK;
}


status_t
PolarisSmu::Start(const char *firmwarePath)
{
	ArrayDeleter<uint8> data;
	off_t size;
	status_t status = ReadFile(firmwarePath, data, size);
	if (status < B_OK) {
		printf("  [!] can't read %s: %s\n", firmwarePath, strerror(status));
		return status;
	}
	const common_firmware_header &header
		= *(const common_firmware_header*)data.Get();
	const uint32 *image
		= (const uint32*)(data.Get() + header.ucode_array_offset_bytes);
	uint32 imageSize = header.ucode_size_bytes;
	if (imageSize > SMU7_SMC_SIZE)
		return B_BAD_DATA;

	uint32 smuFirmware = ReadIndirect(ixSMU_FIRMWARE);
	bool protectionMode = (smuFirmware & SMU_FIRMWARE__SMU_MODE_MASK) != 0;
	printf("SMU:       starting firmware %#" B_PRIx32 " (%" B_PRIu32
		" bytes), %s mode, %s key\n", header.ucode_version, imageSize,
		protectionMode ? "protection" : "non-protection",
		(smuFirmware & SMU_FIRMWARE__SMU_SEL_MASK) != 0 ? "hard" : "soft");

	if (!protectionMode) {
		// polaris10_start_smu_in_non_protection_mode() isn't needed on the
		// cards seen so far
		printf("  [!] non-protection mode not implemented\n");
		return B_NOT_SUPPORTED;
	}

	// *** polaris10_start_smu_in_protection_mode()
	uint32 value = ReadIndirect(ixSMC_SYSCON_RESET_CNTL);
	WriteIndirect(ixSMC_SYSCON_RESET_CNTL,
		value | SMC_SYSCON_RESET_CNTL__rst_reg_MASK);

	// smu7_upload_smc_firmware_data(): to 0x20000, auto increment
	WriteReg4AmdGpu(mmSMC_IND_INDEX_11, 0x20000);
	value = ReadReg4AmdGpu(mmSMC_IND_ACCESS_CNTL);
	WriteReg4AmdGpu(mmSMC_IND_ACCESS_CNTL,
		value | SMC_IND_ACCESS_CNTL__AUTO_INCREMENT_IND_11_MASK);
	for (uint32 i = 0; i < imageSize / 4; i++)
		WriteReg4AmdGpu(mmSMC_IND_DATA_11, image[i]);
	value = ReadReg4AmdGpu(mmSMC_IND_ACCESS_CNTL);
	WriteReg4AmdGpu(mmSMC_IND_ACCESS_CNTL,
		value & ~SMC_IND_ACCESS_CNTL__AUTO_INCREMENT_IND_11_MASK);

	WriteIndirect(ixSMU_STATUS, 0);
	value = ReadIndirect(ixSMC_SYSCON_CLOCK_CNTL_0);
	WriteIndirect(ixSMC_SYSCON_CLOCK_CNTL_0,
		value & ~SMC_SYSCON_CLOCK_CNTL_0__ck_disable_MASK);
	value = ReadIndirect(ixSMC_SYSCON_RESET_CNTL);
	WriteIndirect(ixSMC_SYSCON_RESET_CNTL,
		value & ~SMC_SYSCON_RESET_CNTL__rst_reg_MASK);

	CheckRet(WaitIndirect(ixRCU_UC_EVENTS,
		RCU_UC_EVENTS__INTERRUPTS_ENABLED_MASK,
		RCU_UC_EVENTS__INTERRUPTS_ENABLED_MASK, true, "SMU boot interrupts"));

	// trigger the start with the Test message
	CheckRet(SendMessage(PPSMC_MSG_Test, 0x20000));

	CheckRet(WaitIndirect(ixSMU_STATUS, SMU_STATUS__SMU_DONE_MASK, 0, false,
		"SMU_DONE"));
	if ((ReadIndirect(ixSMU_STATUS) & SMU_STATUS__SMU_PASS_MASK) == 0) {
		printf("  [!] SMU firmware rejected (SMU_STATUS %#010" B_PRIx32 ")\n",
			ReadIndirect(ixSMU_STATUS));
		return B_ERROR;
	}

	WriteIndirect(ixFIRMWARE_FLAGS, 0);
	value = ReadIndirect(ixSMC_SYSCON_RESET_CNTL);
	WriteIndirect(ixSMC_SYSCON_RESET_CNTL,
		value | SMC_SYSCON_RESET_CNTL__rst_reg_MASK);
	value = ReadIndirect(ixSMC_SYSCON_RESET_CNTL);
	WriteIndirect(ixSMC_SYSCON_RESET_CNTL,
		value & ~SMC_SYSCON_RESET_CNTL__rst_reg_MASK);

	CheckRet(WaitIndirect(ixFIRMWARE_FLAGS,
		FIRMWARE_FLAGS__INTERRUPTS_ENABLED_MASK,
		FIRMWARE_FLAGS__INTERRUPTS_ENABLED_MASK, true, "firmware start"));

	printf("SMU:       firmware running (PC %#" B_PRIx32 ")\n",
		ReadIndirect(ixSMC_PC_C));
	fStarted = true;
	return B_OK;
}


status_t
PolarisSmu::LoadUcodes(const char *firmwareDir, const Ucode *ucodes,
	uint32 count)
{
	if (count + 2 > SMU_MAX_ENTRIES)
		return B_BAD_VALUE;

	auto memMgr = gDevice.MemMgr().Switch();

	// amdgpu_ucode_init_bo(): the images without their headers, page
	// aligned; the MEC is followed by a copy of its jump table
	ArrayDeleter<uint8> data[SMU_MAX_ENTRIES];
	uint64 offsets[SMU_MAX_ENTRIES];
	uint64 jtOffsets[SMU_MAX_ENTRIES];
	uint64 total = 0;
	for (uint32 i = 0; i < count; i++) {
		BString path;
		path.SetToFormat("%s/%s", firmwareDir, ucodes[i].file);
		off_t size;
		status_t status = ReadFile(path.String(), data[i], size);
		if (status < B_OK) {
			printf("  [!] can't read %s: %s\n", path.String(), strerror(status));
			return status;
		}
		const common_firmware_header &header
			= *(const common_firmware_header*)data[i].Get();
		offsets[i] = total;
		total += (header.ucode_size_bytes + B_PAGE_SIZE - 1)
			& ~(uint64)(B_PAGE_SIZE - 1);
		jtOffsets[i] = 0;
		if (ucodes[i].id == UCODE_ID_CP_MEC) {
			uint32 jtSize = *(const uint32*)(data[i].Get() + 40) * 4;
			jtOffsets[i] = total;
			total += (jtSize + B_PAGE_SIZE - 1) & ~(uint64)(B_PAGE_SIZE - 1);
		}
	}

	fImages.SetTo(memMgr->Alloc(boDomainVramMappable, total));
	fTocBuffer.SetTo(memMgr->Alloc(boDomainVramMappable, B_PAGE_SIZE));
	fSmuBuffer.SetTo(memMgr->Alloc(boDomainVramMappable, 200 * B_PAGE_SIZE));
	if (fImages.adr == NULL || fTocBuffer.adr == NULL || fSmuBuffer.adr == NULL)
		return B_NO_MEMORY;
	memset(fImages.adr, 0, total);
	memset(fSmuBuffer.adr, 0, fSmuBuffer.buf->size);

	// smu7_request_smu_load_fw(): the TOC lists the MEC jump tables too,
	// but they aren't in the load mask on Polaris
	SMU_DRAMData_TOC toc = {};
	toc.structure_version = 1;
	uint32 mask = 0;
	for (uint32 i = 0; i < count; i++) {
		const common_firmware_header &header
			= *(const common_firmware_header*)data[i].Get();
		const uint8 *ucode = data[i].Get() + header.ucode_array_offset_bytes;
		memcpy((uint8*)fImages.adr + offsets[i], ucode,
			header.ucode_size_bytes);
		uint64 address = fImages.buf->gpuPhysAdr + offsets[i];

		// smu7_populate_single_firmware_entry()
		SMU_Entry &entry = toc.entry[toc.num_entries++];
		entry.id = ucodes[i].id;
		entry.version = (uint16)header.ucode_version;
		entry.image_addr_high = address >> 32;
		entry.image_addr_low = (uint32)address;
		entry.data_size_byte = header.ucode_size_bytes;
		entry.flags = (ucodes[i].id == UCODE_ID_RLC_G
			|| ucodes[i].id == UCODE_ID_CP_MEC) ? 1 : 0;
		mask |= 1u << ucodes[i].id;

		if (ucodes[i].id == UCODE_ID_CP_MEC) {
			// amdgpu_cgs_get_firmware_info(): MEC without the jump table,
			// the jump table (amdgpu_ucode_patch_jt()) as JT1 and JT2
			uint32 jtOffset = *(const uint32*)(data[i].Get() + 36) * 4;
			uint32 jtSize = *(const uint32*)(data[i].Get() + 40) * 4;
			if (jtOffset == 0 || jtOffset + jtSize > header.ucode_size_bytes)
				return B_BAD_DATA;
			entry.data_size_byte = jtOffset;
			memcpy((uint8*)fImages.adr + jtOffsets[i], ucode + jtOffset,
				jtSize);
			uint64 jtAddress = fImages.buf->gpuPhysAdr + jtOffsets[i];
			printf("  %-22s id %2u, version %#06x, %6" B_PRIu32 " bytes at %#"
				B_PRIx64 "\n", ucodes[i].file, entry.id, entry.version,
				entry.data_size_byte, address);
			for (uint16 id = UCODE_ID_CP_MEC_JT1; id <= UCODE_ID_CP_MEC_JT2;
					id++) {
				SMU_Entry &jtEntry = toc.entry[toc.num_entries++];
				jtEntry = entry;
				jtEntry.id = id;
				jtEntry.image_addr_high = jtAddress >> 32;
				jtEntry.image_addr_low = (uint32)jtAddress;
				jtEntry.data_size_byte = jtSize;
				jtEntry.flags = 0;
				printf("  %-22s id %2u, version %#06x, %6" B_PRIu32
					" bytes at %#" B_PRIx64 " (not loaded)\n", "  jump table",
					jtEntry.id, jtEntry.version, jtEntry.data_size_byte,
					jtAddress);
			}
			continue;
		}
		printf("  %-22s id %2u, version %#06x, %6" B_PRIu32 " bytes at %#"
			B_PRIx64 "\n", ucodes[i].file, entry.id, entry.version,
			entry.data_size_byte, address);
	}
	memcpy(fTocBuffer.adr, &toc, sizeof(toc));
	__sync_synchronize();
	WriteReg4AmdGpu(0x1520, 1);		// HDP_MEM_COHERENCY_FLUSH_CNTL
	ReadReg4AmdGpu(0x1520);

	uint32 softRegisters = ReadIndirect(SMU7_FIRMWARE_HEADER_LOCATION
		+ SMU74_FIRMWARE_HEADER_SOFT_REGISTERS);
	uint32 loadStatus = softRegisters + SMU74_SOFT_REGISTERS_UCODE_LOAD_STATUS;
	printf("SMU:       soft registers at %#" B_PRIx32 ", loading mask %#"
		B_PRIx32 "\n", softRegisters, mask);
	WriteIndirect(loadStatus, 0);

	uint64 smuBuffer = fSmuBuffer.buf->gpuPhysAdr;
	CheckRet(SendMessage(PPSMC_MSG_SMU_DRAM_ADDR_HI, smuBuffer >> 32));
	CheckRet(SendMessage(PPSMC_MSG_SMU_DRAM_ADDR_LO, (uint32)smuBuffer));
	uint64 tocAddress = fTocBuffer.buf->gpuPhysAdr;
	CheckRet(SendMessage(PPSMC_MSG_DRV_DRAM_ADDR_HI, tocAddress >> 32));
	CheckRet(SendMessage(PPSMC_MSG_DRV_DRAM_ADDR_LO, (uint32)tocAddress));
	status_t status = SendMessage(PPSMC_MSG_LoadUcodes, mask);

	// smu7_check_fw_load_finish()
	if (status >= B_OK) {
		status = WaitIndirect(loadStatus, mask, mask, true,
			"UcodeLoadStatus");
	}
	printf("SMU:       UcodeLoadStatus %#" B_PRIx32 "\n",
		ReadIndirect(loadStatus));
	if (status >= B_OK)
		fLoadedMask = mask;
	return status;
}


status_t
PolarisSmu::LoadAllFirmware(const char *firmwareDir)
{
	// as Linux, everything in one LoadUcodes, in its TOC order
	static const Ucode kUcodes[] = {
		{UCODE_ID_RLC_G, "polaris11_rlc.bin"},
		{UCODE_ID_CP_CE, "polaris11_ce_2.bin"},
		{UCODE_ID_CP_PFP, "polaris11_pfp_2.bin"},
		{UCODE_ID_CP_ME, "polaris11_me_2.bin"},
		{UCODE_ID_CP_MEC, "polaris11_mec_2.bin"},
		{UCODE_ID_SDMA0, "polaris11_sdma.bin"},
		{UCODE_ID_SDMA1, "polaris11_sdma1.bin"},
	};

	// Like Linux (smu7_request_smu_load_fw() on every driver start), load
	// again even if the SMU loaded the firmware before: a restarted CP
	// without fresh firmware stays busy and loses its EOP interrupts.
	return LoadUcodes(firmwareDir, kUcodes, B_COUNT_OF(kUcodes));
}


uint32
PolarisSmu::Temperature()
{
	// smu7_thermal_get_temperature()
	uint32 temperature = (ReadIndirect(ixCG_MULT_THERMAL_STATUS)
		& CG_MULT_THERMAL_STATUS__CTF_TEMP_MASK)
		>> CG_MULT_THERMAL_STATUS__CTF_TEMP__SHIFT;
	if ((temperature & 0x200) != 0)
		return 255;
	return temperature & 0x1ff;
}


void
PolarisSmu::PrintState()
{
	printf("  SMC_PC_C %#" B_PRIx32 ", SMU_STATUS %#" B_PRIx32
		", SMU_FIRMWARE %#" B_PRIx32 ", FIRMWARE_FLAGS %#" B_PRIx32
		", RCU_UC_EVENTS %#" B_PRIx32 ", %" B_PRIu32 " C\n",
		ReadIndirect(ixSMC_PC_C), ReadIndirect(ixSMU_STATUS),
		ReadIndirect(ixSMU_FIRMWARE), ReadIndirect(ixFIRMWARE_FLAGS),
		ReadIndirect(ixRCU_UC_EVENTS), Temperature());
}
