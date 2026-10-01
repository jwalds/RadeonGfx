#pragma once

// Interface to the render device of the radeon_hd driver
// (graphics/radeon_hd_render_<pci address>). Must match
// headers/private/graphics/radeon_hd/radeon_hd.h in the radeon_hd patches
// (jwalds/haiku-radeon-polaris).

#include <OS.h>
#include <Drivers.h>
#include <graphic_driver.h>

#define RADEON_HD_RENDER_ACCELERANT_NAME	"radeon_gfx.accelerant"
#define RADEON_HD_PRIVATE_DATA_MAGIC		'rdhd'
#define RADEON_HD_GPU_INFO_VERSION			1

enum {
	RADEON_HD_GET_GPU_INFO = B_DEVICE_OP_CODES_END + 5,
};

struct radeon_hd_gpu_info {
	uint32	magic;
	uint32	version;
	area_id	registers_area;
	uint64	registers_size;
	area_id	rom_area;
	uint32	rom_size;
	area_id	frame_buffer_area;
	uint64	frame_buffer_phys;
	uint64	frame_buffer_size;
	uint64	graphics_memory_size;
	uint16	pci_id;
	uint8	pci_revision;
	uint8	dce_major;
	uint8	dce_minor;
	uint16	chipset_id;
};
