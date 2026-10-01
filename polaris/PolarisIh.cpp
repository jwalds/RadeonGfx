#include "PolarisIh.h"
#include "RadeonDevice.h"
#include "Radeon.h"

#include <stdio.h>
#include <string.h>

#include "vi/oss_3_0_d.h"
#include "vi/oss_3_0_sh_mask.h"
#include "vi/bif_5_0_d.h"
#include "vi/bif_5_0_sh_mask.h"

#define SET_FIELD(value, reg, field, fieldValue) \
	(((value) & ~reg##__##field##_MASK) \
		| (((uint32)(fieldValue) << reg##__##field##__SHIFT) \
			& reg##__##field##_MASK))


static const uint32 kSavedRegisters[] = {
	mmINTERRUPT_CNTL,
	mmINTERRUPT_CNTL2,
	mmIH_RB_BASE,
	mmIH_RB_WPTR_ADDR_LO,
	mmIH_RB_WPTR_ADDR_HI,
	mmIH_DOORBELL_RPTR,
	mmIH_RB_CNTL,
};

static uint32 sSavedValues[B_COUNT_OF(kSavedRegisters)];


static uint32
OrderBase2(uint32 value)
{
	uint32 order = 0;
	while ((1u << order) < value)
		order++;
	return order;
}


PolarisIhRing::PolarisIhRing():
	fRingSize(0), fRptr(0), fOverflowed(false), fEnabled(false)
{
}


PolarisIhRing::~PolarisIhRing()
{
	Fini();
}


status_t
PolarisIhRing::Init(uint32 ringSize)
{
	if (!gDevice.RegsWritable())
		return B_NOT_ALLOWED;

	auto memMgr = gDevice.MemMgr().Switch();
	fRing.SetTo(memMgr->Alloc(boDomainVramMappable, ringSize, ringSize));
	fWptr.SetTo(memMgr->Alloc(boDomainVramMappable, B_PAGE_SIZE));
	if (fRing.adr == NULL || fWptr.adr == NULL)
		return B_NO_MEMORY;
	memset(fRing.adr, 0, ringSize);
	memset(fWptr.adr, 0, B_PAGE_SIZE);
	fRingSize = ringSize;
	fRptr = 0;
	fOverflowed = false;

	for (uint32 i = 0; i < B_COUNT_OF(kSavedRegisters); i++)
		sSavedValues[i] = ReadReg4AmdGpu(kSavedRegisters[i]);

	// tonga_ih_disable_interrupts()
	uint32 value = ReadReg4AmdGpu(mmIH_RB_CNTL);
	value = SET_FIELD(value, IH_RB_CNTL, RB_ENABLE, 0);
	value = SET_FIELD(value, IH_RB_CNTL, ENABLE_INTR, 0);
	WriteReg4AmdGpu(mmIH_RB_CNTL, value);
	WriteReg4AmdGpu(mmIH_RB_RPTR, 0);
	WriteReg4AmdGpu(mmIH_RB_WPTR, 0);

	// tonga_ih_irq_init(), ring in VRAM
	WriteReg4AmdGpu(mmINTERRUPT_CNTL2,
		memMgr->fDummyPage->gpuPhysAdr >> 8);
	value = ReadReg4AmdGpu(mmINTERRUPT_CNTL);
	value = SET_FIELD(value, INTERRUPT_CNTL, IH_DUMMY_RD_OVERRIDE, 0);
	// the ring is in non-cacheable memory (VRAM)
	value = SET_FIELD(value, INTERRUPT_CNTL, IH_REQ_NONSNOOP_EN, 1);
	WriteReg4AmdGpu(mmINTERRUPT_CNTL, value);

	WriteReg4AmdGpu(mmIH_RB_BASE, fRing.buf->gpuPhysAdr >> 8);

	value = SET_FIELD(0, IH_RB_CNTL, WPTR_OVERFLOW_CLEAR, 1);
	value = SET_FIELD(value, IH_RB_CNTL, RB_SIZE, OrderBase2(ringSize / 4));
	value = SET_FIELD(value, IH_RB_CNTL, WPTR_WRITEBACK_ENABLE, 1);
	value = SET_FIELD(value, IH_RB_CNTL, MC_VMID, 0);
	WriteReg4AmdGpu(mmIH_RB_CNTL, value);

	WriteReg4AmdGpu(mmIH_RB_WPTR_ADDR_LO, (uint32)fWptr.buf->gpuPhysAdr);
	WriteReg4AmdGpu(mmIH_RB_WPTR_ADDR_HI,
		(uint32)(fWptr.buf->gpuPhysAdr >> 32) & 0xff);
	WriteReg4AmdGpu(mmIH_RB_RPTR, 0);
	WriteReg4AmdGpu(mmIH_RB_WPTR, 0);

	value = ReadReg4AmdGpu(mmIH_DOORBELL_RPTR);
	value = SET_FIELD(value, IH_DOORBELL_RPTR, ENABLE, 0);
	WriteReg4AmdGpu(mmIH_DOORBELL_RPTR, value);

	// tonga_ih_enable_interrupts(), but without the CPU interrupt
	value = ReadReg4AmdGpu(mmIH_RB_CNTL);
	value = SET_FIELD(value, IH_RB_CNTL, RB_ENABLE, 1);
	value = SET_FIELD(value, IH_RB_CNTL, ENABLE_INTR, 0);
	WriteReg4AmdGpu(mmIH_RB_CNTL, value);
	fEnabled = true;

	printf("IH ring:   %" B_PRIu32 " KB at %#" B_PRIx64 ", wptr at %#"
		B_PRIx64 ", polled (CPU interrupt off)\n", ringSize / 1024,
		fRing.buf->gpuPhysAdr, fWptr.buf->gpuPhysAdr);
	return B_OK;
}


void
PolarisIhRing::Fini()
{
	if (!fEnabled)
		return;

	uint32 value = ReadReg4AmdGpu(mmIH_RB_CNTL);
	value = SET_FIELD(value, IH_RB_CNTL, RB_ENABLE, 0);
	value = SET_FIELD(value, IH_RB_CNTL, ENABLE_INTR, 0);
	WriteReg4AmdGpu(mmIH_RB_CNTL, value);
	WriteReg4AmdGpu(mmIH_RB_RPTR, 0);
	WriteReg4AmdGpu(mmIH_RB_WPTR, 0);

	for (int32 i = B_COUNT_OF(kSavedRegisters) - 1; i >= 0; i--)
		WriteReg4AmdGpu(kSavedRegisters[i], sSavedValues[i]);
	fEnabled = false;
}


uint32
PolarisIhRing::Poll(Handler handler, void *cookie)
{
	if (!fEnabled)
		return 0;

	uint32 mask = fRingSize - 1;
	uint32 wptr = *(volatile uint32*)fWptr.adr;
	if ((wptr & IH_RB_WPTR__RB_OVERFLOW_MASK) != 0) {
		// tonga_ih_get_wptr(): continue after the oldest vector that is
		// still intact, then clear the overflow
		fOverflowed = true;
		wptr &= ~IH_RB_WPTR__RB_OVERFLOW_MASK;
		fRptr = (wptr + 16) & mask;
		uint32 value = ReadReg4AmdGpu(mmIH_RB_CNTL);
		WriteReg4AmdGpu(mmIH_RB_CNTL,
			SET_FIELD(value, IH_RB_CNTL, WPTR_OVERFLOW_CLEAR, 1));
		WriteReg4AmdGpu(mmIH_RB_CNTL,
			SET_FIELD(value, IH_RB_CNTL, WPTR_OVERFLOW_CLEAR, 0));
	}
	wptr &= mask;

	uint32 count = 0;
	const volatile uint32 *ring = (const volatile uint32*)fRing.adr;
	while (fRptr != wptr) {
		uint32 index = fRptr / 4;
		Entry entry;
		for (int i = 0; i < 4; i++)
			entry.raw[i] = ring[index + i];
		// tonga_ih_decode_iv()
		entry.srcId = entry.raw[0] & 0xff;
		entry.srcData = entry.raw[1] & 0xfffffff;
		entry.ringId = entry.raw[2] & 0xff;
		entry.vmId = (entry.raw[2] >> 8) & 0xff;
		entry.pasId = (entry.raw[2] >> 16) & 0xffff;
		fRptr = (fRptr + 16) & mask;
		count++;
		if (handler != NULL)
			handler(entry, cookie);
	}
	if (count > 0)
		WriteReg4AmdGpu(mmIH_RB_RPTR, fRptr);
	return count;
}
