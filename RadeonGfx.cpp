#include <stdio.h>
#include <unistd.h>
#include <dirent.h>
#include <strings.h>
#include <FindDirectory.h>
#include <graphic_driver.h>
#include <Accelerant.h>
#include <String.h>
#include <private/shared/AutoDeleter.h>
#include <private/shared/AutoDeleterOS.h>
#include <private/shared/AutoDeleterPosix.h>

#include "RadeonDevice.h"
#include "RadeonServer.h"
#include "PolarisInfo.h"
#include "PolarisMemTest.h"
#include "PolarisGartTest.h"
#include "PolarisIhTest.h"
#include "PolarisSdmaTest.h"
#include "PolarisGfxTest.h"
#include "RenderDevice.h"
#include <string.h>
#include <signal.h>
#include <Application.h>

#define CheckRet(err) {status_t _err = (err); if (_err < B_OK) return _err;}


status_t RadeonTest();

status_t InitDevice()
{
	const char *apath = "/dev/graphics";

	struct dirent	*e;
	char name_buf[1024];

	/* open directory apath */
	DirCloser d(opendir(apath));
	if (!d.IsSet()) return B_ERROR;
	while ((e = readdir(d.Get())) != NULL) {
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		sprintf(name_buf, "%s/%s", apath, e->d_name);
		printf("path: %s\n", name_buf);
		FileDescriptorCloser fd(open(name_buf, B_READ_WRITE));
		char signature[1024];
		if (ioctl(fd.Get(), B_GET_ACCELERANT_SIGNATURE, &signature, sizeof(signature)) < B_OK) {
			printf("B_GET_ACCELERANT_SIGNATURE failed\n");
			continue;
		}
		printf("signature: %s\n", signature);
		if (strcmp(signature, "radeon_gfx.accelerant") == 0) {
			CheckRet(gDevice.Init(fd.Get()));
			return B_OK;
		}
	}
	return B_ERROR;
}


int main(int argc, char** argv)
{
	enum Mode {
		testMode,
		serverMode
	};
	Mode mode = serverMode;

	// unbuffered, so the last step is visible if the machine hangs
	setvbuf(stdout, NULL, _IONBF, 0);

	// read-only probe of a Polaris GPU through the radeon_hd render device
	if (argc >= 2 && strcmp(argv[1], "info") == 0)
		return PolarisInfo() < B_OK ? 1 : 0;
	if (argc >= 2 && strcmp(argv[1], "memtest") == 0)
		return PolarisMemTest() < B_OK ? 1 : 0;
	if (argc >= 2 && strcmp(argv[1], "garttest") == 0)
		return PolarisGartTest() < B_OK ? 1 : 0;
	if (argc >= 2 && strcmp(argv[1], "ihtest") == 0)
		return PolarisIhTest() < B_OK ? 1 : 0;
	if (argc >= 2 && strcmp(argv[1], "sdmatest") == 0)
		return PolarisSdmaTest() < B_OK ? 1 : 0;
	if (argc >= 2 && strcmp(argv[1], "gfxtest") == 0)
		return PolarisGfxTest(argc >= 3
			&& strcmp(argv[2], "--all-vm-contexts") == 0) < B_OK ? 1 : 0;

	// GPU server on Polaris
	if (argc >= 2 && strcmp(argv[1], "server") == 0
		&& (argc < 3 || strcmp(argv[argc - 1], "--si") != 0)) {
		RadeonInitServer();
		// stop the GPU cleanly on ^C or kill: the engines must not keep
		// writing into memory of a dead team
		signal(SIGINT, [](int) {be_app->PostMessage(B_QUIT_REQUESTED);});
		signal(SIGTERM, [](int) {be_app->PostMessage(B_QUIT_REQUESTED);});
		FileDescriptorCloser fd;
		BString path;
		if (OpenRenderDevice(fd, path) < B_OK) {
			fprintf(stderr, "no radeon_hd render device found\n");
			return 1;
		}
		printf("Device:    %s\n", path.String());
		status_t status = gDevice.InitPolarisServer(fd.Get());
		if (status < B_OK) {
			fprintf(stderr, "[!] Polaris server init failed: %s\n",
				strerror(status));
			gDevice.FiniPolarisServer();
			return 1;
		}
		RadeonRunServer();
		gDevice.FiniPolarisServer();
		return 0;
	}

	// The server and test modes still contain Southern Islands (GFX6)
	// initialization, which must not run on Polaris (GFX8).
	if (argc < 3 || strcmp(argv[argc - 1], "--si") != 0) {
		fprintf(stderr, "Only \"%s info|memtest|garttest|ihtest|sdmatest|gfxtest|server\" is supported on Polaris yet "
			"(add --si to run the Southern Islands code).\n", argv[0]);
		return 1;
	}

	if (argc >= 2) {
		if (strcmp(argv[1], "server") == 0) mode = serverMode;
		if (strcmp(argv[1], "test") == 0) mode = testMode;
	}

	/*if (mode == serverMode)*/ RadeonInitServer();

	if (InitDevice() < B_OK) return -1;

	switch (mode) {
		case serverMode:
			RadeonRunServer();
			break;
		case testMode:
			RadeonTest();
			break;
	}
	return 0;
}
