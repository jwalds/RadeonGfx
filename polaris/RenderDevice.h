#pragma once

#include <String.h>
#include <private/shared/AutoDeleterPosix.h>

#include "RadeonHdGpuInfo.h"


// Opens the first radeon_hd render device (graphics/radeon_hd_render_*).
status_t OpenRenderDevice(FileDescriptorCloser &fd, BString &path);

// RADEON_GET_GPU_INFO on an open render device.
status_t GetGpuInfo(int fd, radeon_hd_gpu_info &info);
