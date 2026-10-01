// Memory setup for Polaris (GFX8): VRAM pools that leave out what the
// radeon_hd display driver uses. Doesn't write any register.

#include "RadeonMemory.h"
#include "RadeonDevice.h"
#include "Radeon.h"

#include <stdio.h>
#include <string.h>

#include "vi/gmc_8_1_d.h"
#include "vi/gmc_8_1_sh_mask.h"
#include "vi/bif_5_0_d.h"
#include "vi/dce_11_2_d.h"

#define CheckRet(err) {status_t _err = (err); if (_err < B_OK) return _err;}


// radeon_hd puts the visible screen at the start of VRAM (sized by the
// display mode) and the cursor image in the last page aligned 32 KB of the
// CPU visible VRAM. Reserve generous blocks for both.
static const uint64 kDisplayFrontReserve = 64 * 1024 * 1024;
static const uint64 kDisplayCursorReserve = 1024 * 1024;

// DCE 11.2 display controllers (CRTC/DCP register blocks)
static const uint32 kCrtcOffsets[] = {
	0x0000, 0x0200, 0x0400, 0x2600, 0x2800, 0x2a00
};

#define CRTC_CONTROL__CRTC_MASTER_EN_MASK	0x1
#define GRPH_ENABLE__GRPH_ENABLE_MASK		0x1
#define CUR_CONTROL__CURSOR_EN_MASK			0x1


static bool
InRange(uint64 address, uint64 begin, uint64 size)
{
	return address >= begin && address < begin + size;
}


status_t
MemoryManager::InitPolaris()
{
	const radeon_hd_gpu_info &gpuInfo = gDevice.GpuInfo();

	uint32 fbLocation = ReadReg4AmdGpu(mmMC_VM_FB_LOCATION);
	uint64 vramBase = (uint64)((fbLocation & MC_VM_FB_LOCATION__FB_BASE_MASK)
		>> MC_VM_FB_LOCATION__FB_BASE__SHIFT) << 24;
	uint64 vramTop = ((uint64)((fbLocation & MC_VM_FB_LOCATION__FB_TOP_MASK)
		>> MC_VM_FB_LOCATION__FB_TOP__SHIFT) << 24) | 0xffffff;
	uint64 vramSize = (uint64)ReadReg4AmdGpu(mmCONFIG_MEMSIZE) * 1024 * 1024;
	uint64 mappableSize = gpuInfo.frame_buffer_size;

	printf("VRAM:      %#" B_PRIx64 " - %#" B_PRIx64 " (%" B_PRIu64 " MB)\n",
		vramBase, vramTop, vramSize / (1024 * 1024));
	if (vramSize == 0 || vramBase + vramSize - 1 > vramTop
		|| mappableSize == 0 || mappableSize > vramSize) {
		printf("[!] inconsistent VRAM setup (FB_LOCATION %#" B_PRIx32
			", CONFIG_MEMSIZE %" B_PRIu64 " MB, visible %" B_PRIu64 " MB)\n",
			fbLocation, vramSize / (1024 * 1024),
			mappableSize / (1024 * 1024));
		return B_ERROR;
	}

	fVramRange = {.beg = vramBase, .size = vramSize};
	// GART is set up in a later step; it goes above VRAM in the 40 bit
	// GPU address space
	fGttRange = {.beg = 0xff00000000, .size = 0x80000000};

	fDomainPools[boDomainVramMappable].Register(vramBase, mappableSize);
	fDomainPools[boDomainVram].Register(vramBase + mappableSize,
		vramSize - mappableSize);

	printf("visible:   %#" B_PRIx64 " - %#" B_PRIx64 " (%" B_PRIu64 " MB)\n",
		vramBase, vramBase + mappableSize - 1, mappableSize / (1024 * 1024));

	CheckRet(ReserveDisplayMemory());

	// a zeroed page and a scratch page, as on Southern Islands
	MappedBuffer dummyPage(Alloc(boDomainVramMappable, B_PAGE_SIZE));
	if (dummyPage.adr == NULL) return B_NO_MEMORY;
	memset(dummyPage.adr, 0, dummyPage.buf->size);
	fDummyPage = dummyPage.buf;

	MappedBuffer vramScratch(Alloc(boDomainVramMappable, B_PAGE_SIZE));
	if (vramScratch.adr == NULL) return B_NO_MEMORY;
	memset(vramScratch.adr, 0, vramScratch.buf->size);
	fVramScratch = vramScratch.buf;

	fVmidPool.Register(1, 15);
	return B_OK;
}


/*!	Takes the display driver's VRAM out of the mappable pool, then checks
	that every enabled scanout and cursor surface is inside what was taken.
*/
status_t
MemoryManager::ReserveDisplayMemory()
{
	uint64 vramBase = fVramRange.beg;
	uint64 mappableSize = gDevice.GpuInfo().frame_buffer_size;
	uint64 cursorBase = vramBase + mappableSize - kDisplayCursorReserve;

	fDisplayFront = AllocAt(vramBase, kDisplayFrontReserve);
	fDisplayCursor = AllocAt(cursorBase, kDisplayCursorReserve);
	if (!fDisplayFront.IsSet() || !fDisplayCursor.IsSet()) {
		printf("[!] can't reserve the display memory\n");
		return B_ERROR;
	}
	printf("display:   %#" B_PRIx64 " - %#" B_PRIx64 " (screen), %#" B_PRIx64
		" - %#" B_PRIx64 " (cursor) reserved\n", vramBase,
		vramBase + kDisplayFrontReserve - 1, cursorBase,
		cursorBase + kDisplayCursorReserve - 1);

	bool ok = true;
	for (uint32 crtc = 0; crtc < B_COUNT_OF(kCrtcOffsets); crtc++) {
		uint32 offset = kCrtcOffsets[crtc];
		// the VBIOS leaves the graphics surface enabled (at address 0) on
		// controllers that don't run; only running ones scan out memory
		if ((ReadReg4AmdGpu(mmCRTC_CONTROL + offset)
				& CRTC_CONTROL__CRTC_MASTER_EN_MASK) == 0)
			continue;
		if ((ReadReg4AmdGpu(mmGRPH_ENABLE + offset)
				& GRPH_ENABLE__GRPH_ENABLE_MASK) != 0) {
			uint64 address = ReadReg4AmdGpu(mmGRPH_PRIMARY_SURFACE_ADDRESS + offset)
				| (uint64)ReadReg4AmdGpu(
					mmGRPH_PRIMARY_SURFACE_ADDRESS_HIGH + offset) << 32;
			bool inside = InRange(address, vramBase, kDisplayFrontReserve);
			printf("  CRTC %" B_PRIu32 " scanout at %#" B_PRIx64 "%s\n", crtc,
				address, inside ? "" : " [!] outside the reserved block");
			ok &= inside;
		}
		if ((ReadReg4AmdGpu(mmCUR_CONTROL + offset)
				& CUR_CONTROL__CURSOR_EN_MASK) != 0) {
			uint64 address = ReadReg4AmdGpu(mmCUR_SURFACE_ADDRESS + offset)
				| (uint64)ReadReg4AmdGpu(mmCUR_SURFACE_ADDRESS_HIGH + offset)
					<< 32;
			bool inside = InRange(address, cursorBase, kDisplayCursorReserve);
			printf("  CRTC %" B_PRIu32 " cursor at %#" B_PRIx64 "%s\n", crtc,
				address, inside ? "" : " [!] outside the reserved block");
			ok &= inside;
		}
	}
	return ok ? B_OK : B_ERROR;
}
