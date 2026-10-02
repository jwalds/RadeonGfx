#pragma once

#include <private/shared/AutoDeleterOS.h>
#include "SADomains/Domains.h"
#include "RadeonMemory.h"
#include "RadeonInfo.h"
#include "RadeonHdGpuInfo.h"


enum section_id {
    SECT_NONE,
    SECT_CONTEXT,
    SECT_CLEAR,
    SECT_CTRLCONST
};

struct cs_extent_def {
    const uint32 *extent;
    const uint32 reg_index;
    const uint32 reg_count;
};

struct cs_section_def {
    const struct cs_extent_def *section;
    const enum section_id id;
};


struct radeon_shared_info;
class Atombios;
class DisplayRoster;
class MemoryManager;
class RadeonRingBuffer;
class RadeonRingBufferInt;
class RadeonUnit;
class PolarisSmu;


class RadeonDevice {
private:
	FileDescriptorCloser fFd;
	AreaDeleter fSharedArea;
	AreaDeleter fRegsArea;
	radeon_shared_info *fSharedInfo;
	uint8 *fRegs;

	// Polaris: resources from the radeon_hd render device
	bool fIsPolaris = false;
	bool fRegsWritable = false;
	radeon_hd_gpu_info fGpuInfo{};
	AreaDeleter fFrameBufferArea;
	uint8 *fFrameBuffer = NULL;

	// set at the end of Init(): the destructor shuts the engines down
	bool fSiInitialized = false;
	// Polaris server: engines started by InitPolarisServer()
	bool fPolarisServer = false;
	ObjectDeleter<PolarisSmu> fSmu;

	DoublyLinkedList<RadeonUnit> fUnits;

	ObjectDeleter<Atombios> fAtombios;
	ExternalPtr<MemoryManager> fMemMgr;
#ifdef RADEONGFX_DISPLAY
	ExternalPtr<DisplayRoster> fDisplays;
#endif
	ExternalPtr<RadeonRingBuffer> fRings[8];
	ExternalPtr<RadeonRingBufferInt> fIntRing;

	status_t InitUnits();
	status_t FiniUnits();

public:
	struct Firmware {
		ArrayDeleter<uint8> data;
		off_t size;
	};

	struct {
		Firmware pfp;
		Firmware ce;
		Firmware me;
		Firmware mc;
		Firmware rlc;
	} fFirmwares;

	struct amdgpu_device fInfo{};

	struct {
		const uint32 *regList;
		uint32 regListLen;
		const struct cs_section_def *csData;
		MappedBuffer saveRestoreBuf;
		uint32 clearStateSize;
		MappedBuffer clearStateBuf;
	} fRlc;

	RadeonDevice();
	~RadeonDevice();
	status_t Init(int fd);
	// Attach to a Polaris GPU through the radeon_hd render device. Without
	// writableRegisters the registers are mapped read-only, so any write
	// faults instead of reaching the hardware.
	status_t InitPolaris(int fd, bool writableRegisters);
	// Polaris GPU server: memory, GART and VM contexts, interrupts, firmware
	// through the SMU, the graphics ring, and the configuration for the DRM
	// info queries (polaris/PolarisServer.cpp)
	status_t InitPolarisServer(int fd);
	void FiniPolarisServer();

	radeon_shared_info *SharedInfo() {return fSharedInfo;}
	uint8 *Regs() {return fRegs;}
	uint8 *FrameBuffer() {return fFrameBuffer;}
	bool IsPolaris() {return fIsPolaris;}
	bool RegsWritable() {return fRegsWritable;}
	const radeon_hd_gpu_info &GpuInfo() {return fGpuInfo;}

	void InstallUnit(RadeonUnit *unit);
	void InitRing(int ringId, ExternalPtr<RadeonRingBuffer> ring);

	Atombios &Atom() {return *fAtombios.Get();}
	ExternalPtr<MemoryManager> MemMgr() {return fMemMgr;}
#ifdef RADEONGFX_DISPLAY
	ExternalPtr<DisplayRoster> Displays() {return fDisplays;}
#endif
	ExternalPtr<RadeonRingBuffer> Rings(int ringId) {return fRings[ringId];}
	ExternalPtr<RadeonRingBufferInt> IntRing() {return fIntRing;}
};


extern RadeonDevice gDevice;
