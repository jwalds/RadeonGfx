#pragma once

#include <SupportDefs.h>

// the ioctl macros in drm.h need the BSD/default definitions
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#include <sys/ioctl.h>

extern "C" {
#include <libdrm/amdgpu_drm.h>
}


// Replies to the amdgpu DRM info queries on Polaris, from the configuration
// the server collected (RadeonDevice::fInfo) and the hardware. They return
// 0 or an errno value like the kernel's ioctl.
int PolarisQueryDevInfo(drm_amdgpu_info_device *info, uint32 size);
int PolarisReadMmrReg(uint32 offset, uint32 se, uint32 sh, uint32 *value);
int PolarisQueryHwIp(uint32 type, drm_amdgpu_info_hw_ip *info);
int PolarisQueryFirmware(uint32 type, drm_amdgpu_info_firmware *firmware);
int PolarisQueryMemory(drm_amdgpu_memory_info *memory);
