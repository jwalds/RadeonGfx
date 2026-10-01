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

	// read-only probe of a Polaris GPU through the radeon_hd render device
	if (argc >= 2 && strcmp(argv[1], "info") == 0)
		return PolarisInfo() < B_OK ? 1 : 0;

	// The server and test modes still contain Southern Islands (GFX6)
	// initialization, which must not run on Polaris (GFX8).
	if (argc < 3 || strcmp(argv[argc - 1], "--si") != 0) {
		fprintf(stderr, "Only \"%s info\" is supported on Polaris yet "
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
