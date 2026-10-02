#pragma once

// GFX 8 ring packets (Linux gfx_v8_0.c, gmc_v8_0.c), for any writer with
// Write(uint32): the test ring (PolarisGfx) and the server's ring.

#include "vi/gfx_8_0_d.h"
#include "vi/gmc_8_1_d.h"
#include "vi/vid.h"
#include "vi/clearstate_vi.h"

#ifndef CACHE_FLUSH_AND_INV_TS_EVENT
#define CACHE_FLUSH_AND_INV_TS_EVENT	0x14
#endif

// Polaris 11 (gfx_v8_0_setup_rb() with all render backends)
static const uint32 kPolarisRasterConfig = 0x16000012;
static const uint32 kPolarisRasterConfig1 = 0x00000000;


// gfx_v8_0_cp_gfx_start(): clear state preamble and CE partitions
template<typename Writer>
void
GenClearState(Writer &writer)
{
	writer.Write(PACKET3(PACKET3_PREAMBLE_CNTL, 0));
	writer.Write(PACKET3_PREAMBLE_BEGIN_CLEAR_STATE);

	writer.Write(PACKET3(PACKET3_CONTEXT_CONTROL, 1));
	writer.Write(0x80000000);
	writer.Write(0x80000000);

	for (const cs_section_def *section = vi_cs_data;
			section->section != NULL; section++) {
		if (section->id != SECT_CONTEXT)
			continue;
		for (const cs_extent_def *extent = section->section;
				extent->extent != NULL; extent++) {
			writer.Write(PACKET3(PACKET3_SET_CONTEXT_REG, extent->reg_count));
			writer.Write(extent->reg_index - PACKET3_SET_CONTEXT_REG_START);
			for (uint32 i = 0; i < extent->reg_count; i++)
				writer.Write(extent->extent[i]);
		}
	}

	writer.Write(PACKET3(PACKET3_SET_CONTEXT_REG, 2));
	writer.Write(mmPA_SC_RASTER_CONFIG - PACKET3_SET_CONTEXT_REG_START);
	writer.Write(kPolarisRasterConfig);
	writer.Write(kPolarisRasterConfig1);

	writer.Write(PACKET3(PACKET3_PREAMBLE_CNTL, 0));
	writer.Write(PACKET3_PREAMBLE_END_CLEAR_STATE);

	writer.Write(PACKET3(PACKET3_CLEAR_STATE, 0));
	writer.Write(0);

	writer.Write(PACKET3(PACKET3_SET_BASE, 2));
	writer.Write(PACKET3_BASE_INDEX(CE_PARTITION_BASE));
	writer.Write(0x8000);
	writer.Write(0x8000);
}


// gfx_v8_0_ring_emit_fence_gfx(): flushes the caches, then writes the value
// (6 dwords)
template<typename Writer>
void
GenFenceV8(Writer &writer, uint64 address, uint64 value, bool write64,
	bool interrupt)
{
	writer.Write(PACKET3(PACKET3_EVENT_WRITE_EOP, 4));
	writer.Write(EOP_TCL1_ACTION_EN | EOP_TC_ACTION_EN | EOP_TC_WB_ACTION_EN
		| EVENT_TYPE(CACHE_FLUSH_AND_INV_TS_EVENT) | EVENT_INDEX(5));
	writer.Write((uint32)address & 0xfffffffc);
	writer.Write(((uint32)(address >> 32) & 0xffff)
		| DATA_SEL(write64 ? 2 : 1) | INT_SEL(interrupt ? 2 : 0));
	writer.Write((uint32)value);
	writer.Write((uint32)(value >> 32));
}


// gfx_v8_0_ring_emit_ib_gfx() (4 dwords)
template<typename Writer>
void
GenIbV8(Writer &writer, uint64 address, uint32 dwords, uint32 vmid)
{
	writer.Write(PACKET3(PACKET3_INDIRECT_BUFFER, 2));
	writer.Write((uint32)address & 0xfffffffc);
	writer.Write((uint32)(address >> 32) & 0xffff);
	writer.Write(dwords | (vmid << 24));
}


// gfx_v8_0_ring_emit_wreg() through the PFP (5 dwords)
template<typename Writer>
void
GenWriteRegV8(Writer &writer, uint32 reg, uint32 value)
{
	writer.Write(PACKET3(PACKET3_WRITE_DATA, 3));
	writer.Write(WRITE_DATA_ENGINE_SEL(1) | WRITE_DATA_DST_SEL(0) | WR_CONFIRM);
	writer.Write(reg);
	writer.Write(0);
	writer.Write(value);
}


// gfx_v8_0_ring_emit_vm_flush(): page directory of a VMID, TLB
// invalidation, wait for it, then the PFP syncs to the ME (19 dwords)
template<typename Writer>
void
GenVmFlushV8(Writer &writer, uint32 vmid, uint64 pageDirAddress)
{
	uint32 reg = vmid < 8 ? mmVM_CONTEXT0_PAGE_TABLE_BASE_ADDR + vmid
		: mmVM_CONTEXT8_PAGE_TABLE_BASE_ADDR + vmid - 8;
	GenWriteRegV8(writer, reg, (uint32)(pageDirAddress >> 12));
	GenWriteRegV8(writer, mmVM_INVALIDATE_REQUEST, 1u << vmid);

	writer.Write(PACKET3(PACKET3_WAIT_REG_MEM, 5));
	writer.Write(WAIT_REG_MEM_FUNCTION(0) | WAIT_REG_MEM_ENGINE(0));
	writer.Write(mmVM_INVALIDATE_REQUEST);
	writer.Write(0);
	writer.Write(0);	// reference
	writer.Write(0);	// mask
	writer.Write(0x20);	// poll interval

	writer.Write(PACKET3(PACKET3_PFP_SYNC_ME, 0));
	writer.Write(0);
}
