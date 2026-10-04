#pragma once

#include "Fence.h"
#include <AutoDeleter.h>

class FenceGroup: public Fence {
public:
	union CreateFlags {
		struct {
			uint32 all: 1;
		};
		uint32 val;
	};
	union Flags {
		struct {
			uint32 all: 1;
		};
		uint32 val;
	};

private:
	struct GroupHandler: public Handler {
	public:
		FenceGroup *fGroup;
		BReference<Fence> fFence;
		uint32 fIdx;

		void Do(Fence *fence) override;
		bool Retain() override;
		void Unretain() override;
	};

	Flags fFlags;
	ArrayDeleter<GroupHandler> fHandlers;
	uint32 fCount, fRemainingCount;

	bool TryAcquireReference();

public:
	FenceGroup(BReference<Fence> *fences, uint32 count, CreateFlags flags = {});
	virtual ~FenceGroup();

};
