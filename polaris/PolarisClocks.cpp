#include "PolarisClocks.h"
#include "PolarisPowerPlay.h"
#include "PolarisSmu.h"
#include "RenderDevice.h"
#include "RadeonDevice.h"
#include "Radeon.h"

#include <stdio.h>
#include <string.h>
#include <OS.h>
#include <Path.h>
#include <private/shared/AutoDeleterOS.h>

#include "vi/smu7_ppsmc.h"

#define CheckRet(err) {status_t _err = (err); if (_err < B_OK) return _err;}


status_t
PolarisClocks()
{
	FileDescriptorCloser fd;
	BString path;
	if (OpenRenderDevice(fd, path) < B_OK) {
		printf("no radeon_hd render device found in /dev/graphics\n");
		return B_ENTRY_NOT_FOUND;
	}
	CheckRet(gDevice.InitPolaris(fd.Get(), true));

	const radeon_hd_gpu_info &info = gDevice.GpuInfo();
	void *rom = NULL;
	AreaDeleter romArea(clone_area("radeon hd rom", &rom, B_ANY_ADDRESS,
		B_READ_AREA, info.rom_area));
	if (!romArea.IsSet()) {
		printf("can't clone the VBIOS area: %s\n", strerror(romArea.Get()));
		return romArea.Get();
	}
	PolarisPowerPlay powerPlay;
	status_t status = powerPlay.Init((const uint8*)rom, info.rom_size);
	if (status < B_OK)
		printf("[!] PowerPlay table: %s\n", strerror(status));
	else
		powerPlay.Print();

	BPath firmwareDir;
	CheckRet(PolarisFirmwareDir(firmwareDir));
	BPath smcFirmware(firmwareDir.Path(), "polaris11_smc.bin");
	PolarisSmu smu;
	if (!smu.IsFirmwareRunning())
		CheckRet(smu.Start(smcFirmware.Path()));
	uint32 sclk = 0, mclk = 0;
	smu.SendMessage(PPSMC_MSG_API_GetSclkFrequency, 0, &sclk);
	smu.SendMessage(PPSMC_MSG_API_GetMclkFrequency, 0, &mclk);
	printf("current:   engine clock %" B_PRIu32 " MHz, memory clock %" B_PRIu32
		" MHz, %" B_PRIu32 " C\n", sclk / 100, mclk / 100, smu.Temperature());
	return B_OK;
}
