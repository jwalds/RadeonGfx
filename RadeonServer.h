#pragma once

#include <ServerThreadLink.h>


#define RADEON_GFX_SERVER_SIGNATURE "application/x-vnd.X512-RadeonGfx"

// the server's GPU reset counter (int32), readable by the clients: RADV
// asks after every wait whether its device is lost, and only a reset
// needs a round trip to the server
#define RADEON_GFX_RESET_COUNTER_AREA "radeon_gfx reset counter"

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
