// RadeonGfx server on Polaris: brings up what the DRM emulation needs, in
// the order of the bring-up tests (PolarisGfxTest), and keeps it running.

#include "RadeonDevice.h"
#include "RadeonMemory.h"
#include "RadeonUnit.h"
#include "Radeon.h"
#include "PolarisInterrupts.h"
#include "PolarisSmu.h"
#include "Units/GfxV8Unit.h"

#include <stdio.h>
#include <string.h>

#include "vi/gfx_8_0_d.h"
#include "vi/gfx_8_0_sh_mask.h"
#include "vi/gmc_8_1_d.h"
#include "vi/bif_5_0_d.h"
#include "vi/bif_5_0_sh_mask.h"

#define CheckRet(err) {status_t _err = (err); if (_err < B_OK) return _err;}


// amdgpu_atombios_get_gfx_info() values for Polaris 11, and
// gfx_v8_0_gpu_early_init()
static const uint32 kMaxShaderEngines = 2;
static const uint32 kMaxShPerSe = 1;
static const uint32 kMaxCuPerSh = 8;
static const uint32 kMaxBackendsPerSe = 2;
static const uint32 kMaxTextureChannelCaches = 4;
static const uint32 kMaxGprs = 256;
static const uint32 kMaxGsThreads = 32;
static const uint32 kMaxHwContexts = 8;
// vi.c: external revision of Polaris 11
static const uint32 kExternalRevOffset = 0x5a;


static uint32
BitCount(uint32 value)
{
	return __builtin_popcount(value);
}


static void
SelectSeSh(uint32 se, uint32 sh)
{
	// gfx_v8_0_select_se_sh(); ~0 broadcasts
	uint32 value = GRBM_GFX_INDEX__INSTANCE_BROADCAST_WRITES_MASK;
	if (se == ~0u)
		value |= GRBM_GFX_INDEX__SE_BROADCAST_WRITES_MASK;
	else
		value |= se << GRBM_GFX_INDEX__SE_INDEX__SHIFT;
	if (sh == ~0u)
		value |= GRBM_GFX_INDEX__SH_BROADCAST_WRITES_MASK;
	else
		value |= sh << GRBM_GFX_INDEX__SH_INDEX__SHIFT;
	WriteReg4AmdGpu(mmGRBM_GFX_INDEX, value);
}


/*!	The gfx configuration that Linux keeps in adev->gfx, for the DRM info
	queries: gfx_v8_0_gpu_early_init(), gfx_v8_0_setup_rb() and
	gfx_v8_0_get_cu_info(). Called after the GFX block is initialized (the
	tiling tables are written).
*/
static void
FillGfxInfo(amdgpu_device &info, const radeon_hd_gpu_info &gpuInfo)
{
	info.rev_id = (ReadReg4AmdGpu(mmPCIE_EFUSE4)
		& PCIE_EFUSE4__STRAP_BIF_ATI_REV_ID_MASK)
		>> PCIE_EFUSE4__STRAP_BIF_ATI_REV_ID__SHIFT;
	info.external_rev_id = info.rev_id + kExternalRevOffset;
	(void)gpuInfo;

	amdgpu_gfx_config &config = info.gfx.config;
	config.max_shader_engines = kMaxShaderEngines;
	config.max_sh_per_se = kMaxShPerSe;
	config.max_cu_per_sh = kMaxCuPerSh;
	config.max_backends_per_se = kMaxBackendsPerSe;
	config.max_texture_channel_caches = kMaxTextureChannelCaches;
	config.max_gprs = kMaxGprs;
	config.max_gs_threads = kMaxGsThreads;
	config.max_hw_contexts = kMaxHwContexts;
	config.gs_vgt_table_depth = 32;
	config.gs_prim_buffer_depth = 1792;
	config.gb_addr_config = ReadReg4AmdGpu(mmGB_ADDR_CONFIG);
	config.mc_arb_ramcfg = ReadReg4AmdGpu(mmMC_ARB_RAMCFG);
	for (uint32 i = 0; i < 32; i++)
		config.tile_mode_array[i] = ReadReg4AmdGpu(mmGB_TILE_MODE0 + i);
	for (uint32 i = 0; i < 16; i++) {
		config.macrotile_mode_array[i]
			= ReadReg4AmdGpu(mmGB_MACROTILE_MODE0 + i);
	}

	amdgpu_cu_info &cuInfo = info.gfx.cu_info;
	uint32 cuMask = (1u << kMaxCuPerSh) - 1;
	uint32 rbMask = (1u << (kMaxBackendsPerSe / kMaxShPerSe)) - 1;
	uint32 activeRbs = 0;
	cuInfo.number = 0;
	cuInfo.ao_cu_mask = 0;
	for (uint32 se = 0; se < kMaxShaderEngines; se++) {
		for (uint32 sh = 0; sh < kMaxShPerSe; sh++) {
			SelectSeSh(se, sh);

			uint32 rbDisable = ReadReg4AmdGpu(mmCC_RB_BACKEND_DISABLE);
			uint32 userRbDisable = ReadReg4AmdGpu(mmGC_USER_RB_BACKEND_DISABLE);
			amdgpu_rb_config &rb = config.rb_config[se][sh];
			rb.rb_backend_disable = rbDisable;
			rb.user_rb_backend_disable = userRbDisable;
			rb.raster_config = ReadReg4AmdGpu(mmPA_SC_RASTER_CONFIG);
			rb.raster_config_1 = ReadReg4AmdGpu(mmPA_SC_RASTER_CONFIG_1);
			uint32 disabled = ((rbDisable | userRbDisable)
				& CC_RB_BACKEND_DISABLE__BACKEND_DISABLE_MASK)
				>> CC_RB_BACKEND_DISABLE__BACKEND_DISABLE__SHIFT;
			activeRbs |= (~disabled & rbMask)
				<< ((se * kMaxShPerSe + sh) * BitCount(rbMask));

			uint32 inactive = (ReadReg4AmdGpu(mmCC_GC_SHADER_ARRAY_CONFIG)
				| ReadReg4AmdGpu(mmGC_USER_SHADER_ARRAY_CONFIG))
				>> CC_GC_SHADER_ARRAY_CONFIG__INACTIVE_CUS__SHIFT;
			uint32 bitmap = ~inactive & cuMask;
			cuInfo.bitmap[se][sh] = bitmap;
			// dGPU: all active CUs are always on
			uint32 aoBitmap = 0;
			for (uint32 cu = 0; cu < kMaxCuPerSh; cu++) {
				if ((bitmap & (1u << cu)) != 0) {
					cuInfo.number++;
					aoBitmap |= 1u << cu;
				}
			}
			cuInfo.ao_cu_bitmap[se][sh] = aoBitmap;
			cuInfo.ao_cu_mask |= aoBitmap << (se * 16 + sh * 8);
		}
	}
	SelectSeSh(~0u, ~0u);
	config.backend_enable_mask = activeRbs;
	config.num_rbs = BitCount(activeRbs);
	cuInfo.simd_per_cu = 4;
	cuInfo.max_waves_per_simd = 10;
	cuInfo.wave_front_size = 64;
	cuInfo.max_scratch_slots_per_cu = 32;
	cuInfo.lds_size = 64;

	printf("GFX:       %" B_PRIu32 " CUs (SE0 %#" B_PRIx32 ", SE1 %#" B_PRIx32
		"), render backends %#" B_PRIx32 ", revision %#" B_PRIx32
		" (external %#" B_PRIx32 ")\n", cuInfo.number, cuInfo.bitmap[0][0],
		cuInfo.bitmap[1][0], activeRbs, info.rev_id, info.external_rev_id);
}


status_t
RadeonDevice::InitPolarisServer(int fd)
{
	CheckRet(InitPolaris(fd, true));
	fPolarisServer = true;

	CheckRet(MemMgr().Switch()->InitPolaris());
	CheckRet(MemMgr().Switch()->InitGartPolaris(true));

	ExternalPtr<PolarisRingBufferInt> intRing
		= MakeExternal<PolarisRingBufferInt>();
	if (intRing.Get() == NULL)
		return B_NO_MEMORY;
	fIntRing = ExternalPtr<RadeonRingBufferInt>(intRing);
	CheckRet(intRing.Switch()->InitPolaris());

	BPath firmwareDir;
	CheckRet(PolarisFirmwareDir(firmwareDir));
	BPath smcFirmware(firmwareDir.Path(), "polaris11_smc.bin");
	fSmu.SetTo(new(std::nothrow) PolarisSmu());
	if (!fSmu.IsSet())
		return B_NO_MEMORY;
	if (!fSmu->IsFirmwareRunning())
		CheckRet(fSmu->Start(smcFirmware.Path()));
	CheckRet(fSmu->LoadAllFirmware(firmwareDir.Path()));
	printf("SMU:       %" B_PRIu32 " C\n", fSmu->Temperature());

	InstallUnit(GfxV8UnitNew(this));
	CheckRet(InitUnits());

	FillGfxInfo(fInfo, fGpuInfo);
	printf("server:    Polaris ready\n");
	return B_OK;
}


void
RadeonDevice::FiniPolarisServer()
{
	if (!fPolarisServer)
		return;
	fPolarisServer = false;

	FiniUnits();
	if (fIntRing.Get() != NULL) {
		// stops the polling thread; must not hold the ring's lock
		static_cast<PolarisRingBufferInt*>(fIntRing.Get())->FiniPolaris();
	}
	MemMgr().Switch()->FiniGartPolaris();
	if (fSmu.IsSet())
		printf("SMU:       %" B_PRIu32 " C\n", fSmu->Temperature());
	printf("server:    Polaris stopped\n");
}
