// amdgpu DRM info queries on Polaris, as Linux amdgpu_kms.c answers them for
// a Polaris 11 (VI family).

#include "PolarisDrmInfo.h"
#include "PolarisSmu.h"
#include "RadeonDevice.h"
#include "RadeonFirmware.h"
#include "RadeonMemory.h"
#include "Radeon.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <File.h>
#include <Path.h>

#include "vi/gfx_8_0_d.h"
#include "vi/gmc_8_1_d.h"
#include "vi/oss_3_0_d.h"


// virtual address space of a process (AddressSpace): 4096 page directory
// entries of 2 MB
static const uint64 kVirtualAddressOffset = 0x200000;
static const uint64 kVirtualAddressMax
	= (uint64)AddressSpace::pageDirLen * AddressSpace::pageTableLen
		* B_PAGE_SIZE;
// GPU timestamps: 25 MHz reference clock (amdgpu_asic_get_xclk(), in kHz)
static const uint32 kGpuCounterFrequency = 25000;
// Radeon RX 560 boost clocks; the GPU still runs at boot clocks (no
// power management yet)
static const uint32 kMaxEngineClock = 1275000;
static const uint32 kMaxMemoryClock = 1750000;
static const uint32 kVramTypeGddr5 = 5;
static const uint32 kVramBitWidth = 128;


static uint32
RegisterIndex(uint32 offset, uint32 base, uint32 count)
{
	return offset - base < count ? offset - base : ~0u;
}


int
PolarisQueryDevInfo(drm_amdgpu_info_device *info, uint32 size)
{
	if (size < sizeof(drm_amdgpu_info_device))
		memset(info, 0, size);
	else
		memset(info, 0, sizeof(drm_amdgpu_info_device));
	if (size < offsetof(drm_amdgpu_info_device, cu_ao_bitmap))
		return EINVAL;

	const amdgpu_device &device = gDevice.fInfo;
	const amdgpu_gfx_config &config = device.gfx.config;
	const amdgpu_cu_info &cuInfo = device.gfx.cu_info;

	info->device_id = gDevice.GpuInfo().pci_id;
	info->chip_rev = device.rev_id;
	info->external_rev = device.external_rev_id;
	info->pci_rev = gDevice.GpuInfo().pci_revision;
	info->family = AMDGPU_FAMILY_VI;
	info->num_shader_engines = config.max_shader_engines;
	info->num_shader_arrays_per_engine = config.max_sh_per_se;
	info->gpu_counter_freq = kGpuCounterFrequency;
	info->max_engine_clock = kMaxEngineClock;
	info->max_memory_clock = kMaxMemoryClock;
	info->cu_active_number = cuInfo.number;
	info->cu_ao_mask = cuInfo.ao_cu_mask;
	memcpy(info->cu_bitmap, cuInfo.bitmap, sizeof(info->cu_bitmap));
	info->enabled_rb_pipes_mask = config.backend_enable_mask;
	info->num_rb_pipes = config.max_backends_per_se * config.max_shader_engines;
	info->num_hw_gfx_contexts = config.max_hw_contexts;
	info->ids_flags = 0;
	info->virtual_address_offset = kVirtualAddressOffset;
	info->virtual_address_max = kVirtualAddressMax;
	info->virtual_address_alignment = B_PAGE_SIZE;
	info->pte_fragment_size = 64 * 1024;
	info->gart_page_size = B_PAGE_SIZE;
	info->ce_ram_size = 0x8000;
	info->vram_type = kVramTypeGddr5;
	info->vram_bit_width = kVramBitWidth;
	info->vce_harvest_config = 0;
	info->gc_double_offchip_lds_buf = 1;
	info->prim_buf_gpu_addr = 0;
	info->pos_buf_gpu_addr = 0;
	info->cntl_sb_buf_gpu_addr = 0;
	info->param_buf_gpu_addr = 0;
	info->prim_buf_size = 0;
	info->pos_buf_size = 0;
	info->cntl_sb_buf_size = 0;
	info->param_buf_size = 0;
	info->wave_front_size = cuInfo.wave_front_size;
	info->num_shader_visible_vgprs = config.max_gprs;
	info->num_cu_per_sh = config.max_cu_per_sh;
	info->num_tcc_blocks = config.max_texture_channel_caches;
	info->gs_vgt_table_depth = config.gs_vgt_table_depth;
	info->gs_prim_buffer_depth = config.gs_prim_buffer_depth;
	info->max_gs_waves_per_vgt = config.max_gs_threads;
	if (size >= sizeof(drm_amdgpu_info_device))
		memcpy(info->cu_ao_bitmap, cuInfo.ao_cu_bitmap,
			sizeof(info->cu_ao_bitmap));
	return 0;
}


int
PolarisReadMmrReg(uint32 offset, uint32 se, uint32 sh, uint32 *value)
{
	// se/sh: ~0 for "any"; the configuration is the same everywhere
	uint32 seIndex = se == ~0u ? 0 : se;
	uint32 shIndex = sh == ~0u ? 0 : sh;
	const amdgpu_gfx_config &config = gDevice.fInfo.gfx.config;
	if (seIndex >= config.max_shader_engines || shIndex >= config.max_sh_per_se)
		return EINVAL;
	const amdgpu_rb_config &rb = config.rb_config[seIndex][shIndex];

	uint32 index;
	if ((index = RegisterIndex(offset, mmGB_TILE_MODE0, 32)) != ~0u) {
		*value = config.tile_mode_array[index];
		return 0;
	}
	if ((index = RegisterIndex(offset, mmGB_MACROTILE_MODE0, 16)) != ~0u) {
		*value = config.macrotile_mode_array[index];
		return 0;
	}

	switch (offset) {
		case mmCC_RB_BACKEND_DISABLE:
			*value = rb.rb_backend_disable;
			return 0;
		case mmGC_USER_RB_BACKEND_DISABLE:
			*value = rb.user_rb_backend_disable;
			return 0;
		case mmPA_SC_RASTER_CONFIG:
			*value = rb.raster_config;
			return 0;
		case mmPA_SC_RASTER_CONFIG_1:
			*value = rb.raster_config_1;
			return 0;
		case mmGB_ADDR_CONFIG:
			*value = config.gb_addr_config;
			return 0;
		case mmMC_ARB_RAMCFG:
			*value = config.mc_arb_ramcfg;
			return 0;

		// status registers, read live (vi_allowed_read_registers)
		case mmGRBM_STATUS:
		case mmGRBM_STATUS2:
		case mmGRBM_STATUS_SE0:
		case mmGRBM_STATUS_SE1:
		case mmGRBM_STATUS_SE2:
		case mmGRBM_STATUS_SE3:
		case mmSRBM_STATUS:
		case mmSRBM_STATUS2:
		case mmSRBM_STATUS3:
		case mmCP_STAT:
		case mmCP_STALLED_STAT1:
		case mmCP_STALLED_STAT2:
		case mmCP_STALLED_STAT3:
		case mmCP_CPF_BUSY_STAT:
		case mmCP_CPF_STALLED_STAT1:
		case mmCP_CPF_STATUS:
		case mmCP_CPC_BUSY_STAT:
		case mmCP_CPC_STALLED_STAT1:
		case mmCP_CPC_STATUS:
			*value = ReadReg4AmdGpu(offset);
			return 0;
	}
	printf("[!] DRM: read of register %#" B_PRIx32 " not allowed\n", offset);
	return EINVAL;
}


int
PolarisQueryHwIp(uint32 type, drm_amdgpu_info_hw_ip *info)
{
	memset(info, 0, sizeof(*info));
	switch (type) {
		case AMDGPU_HW_IP_GFX:
			// gfx_v8_0_ip_block
			info->hw_ip_version_major = 8;
			info->hw_ip_version_minor = 0;
			info->ib_start_alignment = 32;
			info->ib_size_alignment = 32;
			info->available_rings = 1;
			return 0;
		case AMDGPU_HW_IP_COMPUTE:
			// no compute (MEC) rings yet
			info->hw_ip_version_major = 8;
			info->hw_ip_version_minor = 0;
			info->ib_start_alignment = 32;
			info->ib_size_alignment = 32;
			return 0;
		case AMDGPU_HW_IP_DMA:
			// no SDMA rings for command submission yet
			info->hw_ip_version_major = 3;
			info->hw_ip_version_minor = 0;
			info->ib_start_alignment = 256;
			info->ib_size_alignment = 4;
			return 0;
		case AMDGPU_HW_IP_UVD:
		case AMDGPU_HW_IP_VCE:
		case AMDGPU_HW_IP_UVD_ENC:
		case AMDGPU_HW_IP_VCN_DEC:
		case AMDGPU_HW_IP_VCN_ENC:
		case AMDGPU_HW_IP_VCN_JPEG:
			return 0;
	}
	return EINVAL;
}


static status_t
ReadFirmwareVersion(const char *name, uint32 &version, uint32 &feature)
{
	BPath path;
	status_t status = PolarisFirmwareDir(path);
	if (status < B_OK)
		return status;
	path.Append(name);
	BFile file(path.Path(), B_READ_ONLY);
	gfx_firmware_header_v1_0 header;
	if (file.ReadAt(0, &header, sizeof(header)) != (ssize_t)sizeof(header))
		return B_IO_ERROR;
	version = B_LENDIAN_TO_HOST_INT32(header.header.ucode_version);
	feature = B_LENDIAN_TO_HOST_INT32(header.ucode_feature_version);
	return B_OK;
}


int
PolarisQueryFirmware(uint32 type, drm_amdgpu_info_firmware *firmware)
{
	const char *name = NULL;
	switch (type) {
		case AMDGPU_INFO_FW_GFX_ME: name = "polaris11_me_2.bin"; break;
		case AMDGPU_INFO_FW_GFX_PFP: name = "polaris11_pfp_2.bin"; break;
		case AMDGPU_INFO_FW_GFX_CE: name = "polaris11_ce_2.bin"; break;
		case AMDGPU_INFO_FW_GFX_MEC: name = "polaris11_mec_2.bin"; break;
		case AMDGPU_INFO_FW_GFX_RLC: name = "polaris11_rlc.bin"; break;
		case AMDGPU_INFO_FW_SDMA: name = "polaris11_sdma.bin"; break;
		default:
			// UVD, VCE, SMC, ...: not loaded
			firmware->ver = 0;
			firmware->feature = 0;
			return 0;
	}
	uint32 version, feature;
	if (ReadFirmwareVersion(name, version, feature) < B_OK)
		return EIO;
	firmware->ver = version;
	firmware->feature = feature;
	return 0;
}


int
PolarisQueryMemory(drm_amdgpu_memory_info *memory)
{
	memset(memory, 0, sizeof(*memory));
	uint64 vramTotal, vramUsed, mappableTotal, mappableUsed, gttTotal,
		gttUsed;
	{
		auto memMgr = gDevice.MemMgr().Switch();
		memMgr->GetUsage(vramTotal, vramUsed, boDomainVram);
		memMgr->GetUsage(mappableTotal, mappableUsed, boDomainVramMappable);
		memMgr->GetUsage(gttTotal, gttUsed, boDomainGtt);
	}
	memory->vram.total_heap_size = vramTotal + mappableTotal;
	memory->vram.usable_heap_size = vramTotal + mappableTotal;
	memory->vram.heap_usage = vramUsed + mappableUsed;
	memory->vram.max_allocation = vramTotal * 3 / 4;
	memory->cpu_accessible_vram.total_heap_size = mappableTotal;
	memory->cpu_accessible_vram.usable_heap_size = mappableTotal;
	memory->cpu_accessible_vram.heap_usage = mappableUsed;
	memory->cpu_accessible_vram.max_allocation = mappableTotal * 3 / 4;
	memory->gtt.total_heap_size = gttTotal;
	memory->gtt.usable_heap_size = gttTotal;
	memory->gtt.heap_usage = gttUsed;
	memory->gtt.max_allocation = gttTotal * 3 / 4;
	return 0;
}
