// "RadeonGfx ihtest": enable the interrupt ring and, as a source, the
// vertical blank interrupt of display controller 0, then count the vectors
// for two seconds. All registers are restored afterwards.

#include "PolarisIhTest.h"
#include "PolarisIh.h"
#include "RenderDevice.h"
#include "RadeonDevice.h"
#include "Radeon.h"

#include <stdio.h>
#include <string.h>
#include <OS.h>

#include "vi/dce_11_2_d.h"
#include "vi/dce_11_2_sh_mask.h"

#define CheckRet(err) {status_t _err = (err); if (_err < B_OK) return _err;}

// legacy interrupt source of display controller 1 (D1): vblank and vline
static const uint32 kSrcIdD1 = 1;


struct IhTestState {
	uint32 vblankCount;
	uint32 otherCount;
	uint32 printed;
};


static void
HandleEntry(const PolarisIhRing::Entry &entry, void *cookie)
{
	IhTestState &state = *(IhTestState*)cookie;
	if (state.printed < 3) {
		printf("  vector %08" B_PRIx32 " %08" B_PRIx32 " %08" B_PRIx32 " %08"
			B_PRIx32 ": source %" B_PRIu32 ", data %" B_PRIu32 ", ring %"
			B_PRIu32 ", vmid %" B_PRIu32 "\n", entry.raw[0], entry.raw[1],
			entry.raw[2], entry.raw[3], entry.srcId, entry.srcData,
			entry.ringId, entry.vmId);
		state.printed++;
	}
	if (entry.srcId == kSrcIdD1 && entry.srcData == 0) {
		state.vblankCount++;
		// dce_v11_0_crtc_vblank_int_ack()
		WriteReg4AmdGpu(mmLB_VBLANK_STATUS, LB_VBLANK_STATUS__VBLANK_ACK_MASK);
	} else
		state.otherCount++;
}


status_t
PolarisIhTest()
{
	FileDescriptorCloser fd;
	BString path;
	if (OpenRenderDevice(fd, path) < B_OK) {
		printf("no radeon_hd render device found in /dev/graphics\n");
		return B_ENTRY_NOT_FOUND;
	}
	status_t status = gDevice.InitPolaris(fd.Get(), true);
	if (status < B_OK) {
		printf("[!] attaching to %s failed: %s\n", path.String(),
			strerror(status));
		return status;
	}
	printf("Device:    %s (registers writable)\n", path.String());
	CheckRet(gDevice.MemMgr().Switch()->InitPolaris());

	// the GPU reaches VRAM and the ring only with the GART set up
	status = gDevice.MemMgr().Switch()->InitGartPolaris();
	if (status < B_OK) {
		gDevice.MemMgr().Switch()->FiniGartPolaris();
		return status;
	}

	PolarisIhRing ring;
	status = ring.Init();
	if (status < B_OK) {
		ring.Fini();
		gDevice.MemMgr().Switch()->FiniGartPolaris();
		return status;
	}

	uint32 savedMask = ReadReg4AmdGpu(mmLB_INTERRUPT_MASK);
	WriteReg4AmdGpu(mmLB_VBLANK_STATUS, LB_VBLANK_STATUS__VBLANK_ACK_MASK);
	WriteReg4AmdGpu(mmLB_INTERRUPT_MASK,
		savedMask | LB_INTERRUPT_MASK__VBLANK_INTERRUPT_MASK_MASK);
	printf("D1 vblank interrupt enabled, polling for 2 seconds\n");

	IhTestState state = {};
	bigtime_t start = system_time();
	while (system_time() - start < 2000000) {
		ring.Poll(HandleEntry, &state);
		snooze(1000);
	}
	bigtime_t elapsed = system_time() - start;

	printf("before disabling:\n");
	printf("  DISP_INTERRUPT_STATUS %#010" B_PRIx32 ", LB_VBLANK_STATUS %#010"
		B_PRIx32 ", LB_INTERRUPT_MASK %#010" B_PRIx32 "\n",
		ReadReg4AmdGpu(mmDISP_INTERRUPT_STATUS),
		ReadReg4AmdGpu(mmLB_VBLANK_STATUS),
		ReadReg4AmdGpu(mmLB_INTERRUPT_MASK));
	ring.PrintState();

	WriteReg4AmdGpu(mmLB_INTERRUPT_MASK, savedMask);
	WriteReg4AmdGpu(mmLB_VBLANK_STATUS, LB_VBLANK_STATUS__VBLANK_ACK_MASK);
	snooze(50000);
	ring.Poll(HandleEntry, &state);
	ring.Fini();
	gDevice.MemMgr().Switch()->FiniGartPolaris();

	printf("%" B_PRIu32 " vblank vectors in %.2f s (%.1f per second), %"
		B_PRIu32 " other vectors%s\n", state.vblankCount, elapsed / 1e6,
		state.vblankCount / (elapsed / 1e6), state.otherCount,
		ring.Overflowed() ? ", ring overflowed" : "");
	printf("IH ring and GART disabled, registers restored\n");
	return state.vblankCount > 0 ? B_OK : B_ERROR;
}
