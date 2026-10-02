#pragma once

#include <SupportDefs.h>
#include <AutoDeleter.h>
#include "RadeonMemory.h"
#include "SADomains/Domains.h"

enum {
	intSrcIdProtFault  = 146,
	intSrcIdProtFault2 = 147, // ?
	intSrcIdRingGfx    = 176,
	intSrcIdRingCp1    = 177,
	intSrcIdRingCp2    = 178,
	intSrcIdCpEop      = 181,
	intSrcIdRingDma0   = 224,
	intSrcIdGuiIdle    = 233,
	intSrcIdRingDma1   = 244,
};

struct InterruptPacket {
	uint32 clientId;
	uint32 srcId;
	uint32 srcData;
	uint32 ringId;
	uint32 vmId;
};

typedef void (*InterruptHandler)(void *arg, InterruptPacket &pkt);


class RadeonRingBufferInt: public Object {
private:
	enum {
		srcIdCnt = 256,
	};

	struct HandlerEntry {
		InterruptHandler handler;
		void *arg;
	};

	MappedBuffer fBuffer;
	bool fEnabled;
	uint32 fRptr, fWptr;
	vuint32 *fWptrAdr;
	uint64 fWptrGpuAdr;
	HandlerEntry fHandlers[srcIdCnt];

	thread_id fThread;
	bool fRun;

	status_t ThreadEntry();

protected:
	status_t StartThread();
	void StopThread();
	// hands a vector to its handler, or prints it
	void Dispatch(InterruptPacket &pkt);
	// SI: the kernel driver's interrupt semaphore; B_TIMED_OUT if none came
	virtual status_t WaitForInterrupt();

public:
	RadeonRingBufferInt();
	virtual ~RadeonRingBufferInt();
	status_t Init(uint64 size);
	void Enable();
	void Disable();

	void InstallHandler(uint32 clientId, uint32 srcId, InterruptHandler handler, void *arg);
	void UninstallHandler(uint32 clientId, uint32 srcId);

	uint32 Rptr();
	uint32 Wptr();
	void SetRptr();
	void Read(uint32 &val);
	void ReadPacket(InterruptPacket &pkt);

	virtual bool Handle();
};
