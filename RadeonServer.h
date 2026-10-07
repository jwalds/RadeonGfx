#pragma once

#include <ServerThreadLink.h>


#define RADEON_GFX_SERVER_SIGNATURE "application/x-vnd.X512-RadeonGfx"

// the server's GPU reset counter (int32), readable by the clients: RADV
// asks after every wait whether its device is lost, and only a reset
// needs a round trip to the server
#define RADEON_GFX_RESET_COUNTER_AREA "radeon_gfx reset counter"

// the clients' clone of the server's CPU visible VRAM, kept for the process'
// lifetime; libdrm's amdgpu_bo_cpu_unmap() must not delete it (the same
// name is in libdrm2's amdgpu_bo.c)
#define RADEON_GFX_VRAM_CLONE_NAME "radeon_gfx vram clone"

// per client team: the last signaled point (uint64) of each syncobj handle
// below RADEON_GFX_SYNCOBJ_POINTS_COUNT, read by the client to answer a
// timeline wait for points already reached without a round trip (Linux'
// ioctl returns at once there). Name: the prefix and the team id.
#define RADEON_GFX_SYNCOBJ_POINTS_AREA "radeon_gfx syncobj points "
#define RADEON_GFX_SYNCOBJ_POINTS_COUNT (16 * B_PAGE_SIZE / sizeof(uint64))

enum {
	// DRM
	radeonMmapMsg = userMsgBase,
	radeonIoctlMsg,

	radeonListTeams,
	radeonListBuffers,
	radeonGetMemoryUsage,

	radeonThermalQuery,
	radeonSetClocks,
	// display
	radeonGetDisplayConsumer,
	radeonUpdateCursor,

	radeonBufferDup,
};

enum RadeonHandleType {
	radeonHandleBuffer,
	radeonHandleSyncobj,
};

void RadeonInitServer();
void RadeonRunServer();
