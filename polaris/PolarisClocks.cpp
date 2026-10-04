#include "PolarisClocks.h"
#include "Atombios.h"
#include "PolarisDpm.h"
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


static void
PrintCurrentClocks(PolarisSmu &smu)
{
	uint32 sclk = 0, mclk = 0;
	smu.SendMessage(PPSMC_MSG_API_GetSclkFrequency, 0, &sclk);
	smu.SendMessage(PPSMC_MSG_API_GetMclkFrequency, 0, &mclk);
	printf("current:   engine clock %" B_PRIu32 " MHz, memory clock %" B_PRIu32
		" MHz, %" B_PRIu32 " C\n", sclk / 100, mclk / 100, smu.Temperature());
}


status_t
PolarisClocks(const char *action)
{
	FileDescriptorCloser fd;
	BString path;
	if (OpenRenderDevice(fd, path) < B_OK) {
		printf("no radeon_hd render device found in /dev/graphics\n");
		return B_ENTRY_NOT_FOUND;
	}
	CheckRet(gDevice.InitPolaris(fd.Get(), true));

	if (action != NULL && strcmp(action, "watch") == 0) {
		// only the SMC's current clocks, every half second for 15 s
		PolarisSmu smu;
		if (!smu.IsFirmwareRunning()) {
			printf("the SMC firmware isn't running\n");
			return B_NOT_INITIALIZED;
		}
		for (int i = 0; i < 30; i++) {
			PrintCurrentClocks(smu);
			snooze(500000);
		}
		return B_OK;
	}

	const radeon_hd_gpu_info &info = gDevice.GpuInfo();
	void *rom = NULL;
	AreaDeleter romArea(clone_area("radeon hd rom", &rom, B_ANY_ADDRESS,
		B_READ_AREA, info.rom_area));
	if (!romArea.IsSet()) {
		printf("can't clone the VBIOS area: %s\n", strerror(romArea.Get()));
		return romArea.Get();
	}
	PolarisPowerPlay powerPlay;
	CheckRet(powerPlay.Init((const uint8*)rom, info.rom_size));
	Atombios atombios;
	CheckRet(atombios.Init(info.rom_area));
	status_t status = powerPlay.ResolveVoltages(atombios.Context());
	powerPlay.Print();
	if (status < B_OK) {
		printf("[!] EVV voltages: %s\n", strerror(status));
		return status;
	}

	BPath firmwareDir;
	CheckRet(PolarisFirmwareDir(firmwareDir));
	BPath smcFirmware(firmwareDir.Path(), "polaris11_smc.bin");
	PolarisSmu smu;
	if (!smu.IsFirmwareRunning())
		CheckRet(smu.Start(smcFirmware.Path()));

	PolarisDpm dpm(smu, powerPlay, atombios, (const uint8*)rom,
		info.rom_size);
	CheckRet(dpm.Init());
	dpm.Print();
	CheckRet(dpm.BuildTable());
	dpm.PrintTable();
	printf("DPM %s\n", dpm.IsRunning() ? "running" : "not running");
	PrintCurrentClocks(smu);

	if (action == NULL)
		return B_OK;
	bool start = strcmp(action, "start") == 0;
	bool startMemory = strcmp(action, "start-memory") == 0;
	if (!start && !startMemory && strcmp(action, "upload") != 0) {
		printf("unknown action %s (upload, start, start-memory)\n", action);
		return B_BAD_VALUE;
	}

	printf("uploading the DPM table\n");
	CheckRet(dpm.Upload());
	if (!start && !startMemory)
		return B_OK;

	printf("starting DPM%s\n", startMemory ? " with memory clocks" : "");
	CheckRet(dpm.Start(startMemory));
	printf("DPM %s\n", dpm.IsRunning() ? "running" : "not running");
	for (int i = 0; i < 5; i++) {
		snooze(500000);
		PrintCurrentClocks(smu);
	}
	return B_OK;
}
