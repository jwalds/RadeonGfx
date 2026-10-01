#include "RenderDevice.h"

#include <dirent.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <private/shared/AutoDeleter.h>


static const char kRenderDevicePrefix[] = "radeon_hd_render_";


status_t
OpenRenderDevice(FileDescriptorCloser &fd, BString &path)
{
	const char *dirPath = "/dev/graphics";
	DirCloser dir(opendir(dirPath));
	if (!dir.IsSet())
		return B_ENTRY_NOT_FOUND;

	struct dirent *entry;
	while ((entry = readdir(dir.Get())) != NULL) {
		// only open our render nodes, opening other drivers has side effects
		if (strncmp(entry->d_name, kRenderDevicePrefix,
				sizeof(kRenderDevicePrefix) - 1) != 0)
			continue;

		BString name;
		name.SetToFormat("%s/%s", dirPath, entry->d_name);
		FileDescriptorCloser deviceFd(open(name.String(), B_READ_WRITE));
		if (!deviceFd.IsSet())
			continue;

		char signature[B_PATH_NAME_LENGTH];
		if (ioctl(deviceFd.Get(), B_GET_ACCELERANT_SIGNATURE, signature,
				sizeof(signature)) < B_OK
			|| strcmp(signature, RADEON_HD_RENDER_ACCELERANT_NAME) != 0)
			continue;

		fd.SetTo(deviceFd.Detach());
		path = name;
		return B_OK;
	}
	return B_ENTRY_NOT_FOUND;
}


status_t
GetGpuInfo(int fd, radeon_hd_gpu_info &info)
{
	info = {};
	info.magic = RADEON_HD_PRIVATE_DATA_MAGIC;
	if (ioctl(fd, RADEON_HD_GET_GPU_INFO, &info, sizeof(info)) < B_OK)
		return B_ERROR;
	if (info.version != RADEON_HD_GPU_INFO_VERSION)
		return B_BAD_VALUE;
	return B_OK;
}
