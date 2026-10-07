#include "Test.h"

#include "Fence.h"
#include "Syncobj.h"


// The last signaled point a syncobj publishes for its clients
// (RADEON_GFX_SYNCOBJ_POINTS_AREA): the accelerant answers a timeline wait
// from it without asking the server, so it must never be ahead of Wait().


static BReference<Fence>
NewFence()
{
	return BReference<Fence>(new Fence(), true);
}


TEST(Syncobj, PublishesSignaledPoints)
{
	SyncobjRef syncobj(new Syncobj(), true);
	volatile uint64 slot = 99;
	syncobj->PublishTo(&slot);
	CHECK_EQ(slot, 0);

	BReference<Fence> fence1 = NewFence(), fence2 = NewFence();
	CHECK_EQ(syncobj->InsertFence(1, fence1), B_OK);
	CHECK_EQ(syncobj->InsertFence(2, fence2), B_OK);
	// submitted isn't signaled
	CHECK_EQ(slot, 0);
	fence1->Signal();
	CHECK_EQ(slot, 1);
	fence2->Signal();
	CHECK_EQ(slot, 2);
	syncobj->UnpublishFrom(&slot);
}


TEST(Syncobj, HigherPointSignaledFirst)
{
	SyncobjRef syncobj(new Syncobj(), true);
	volatile uint64 slot = 0;
	syncobj->PublishTo(&slot);
	BReference<Fence> fence1 = NewFence(), fence2 = NewFence();
	syncobj->InsertFence(1, fence1);
	syncobj->InsertFence(2, fence2);
	fence2->Signal();
	CHECK_EQ(slot, 2);
	// point 1 counts as signaled now: Wait() returns at once for it too
	uint64 point = 1;
	uint32 first = 0;
	CHECK_EQ(Syncobj::Wait(&first, &syncobj, &point, 1, {}, 0), B_OK);
	// the lower point's fence was dropped, its signal changes nothing
	fence1->Signal();
	CHECK_EQ(slot, 2);
	syncobj->UnpublishFrom(&slot);
}


TEST(Syncobj, PublishToShowsCurrentPoint)
{
	SyncobjRef syncobj(new Syncobj(), true);
	CHECK_EQ(syncobj->Signal(5), B_OK);
	volatile uint64 a = 0, b = 0;
	syncobj->PublishTo(&a);
	syncobj->PublishTo(&b);
	// twice is once
	syncobj->PublishTo(&a);
	CHECK_EQ(a, 5);
	CHECK_EQ(b, 5);
	syncobj->Signal(7);
	CHECK_EQ(a, 7);
	CHECK_EQ(b, 7);
	syncobj->UnpublishFrom(&a);
	syncobj->UnpublishFrom(&b);
}


TEST(Syncobj, ResetAndUnpublish)
{
	SyncobjRef syncobj(new Syncobj(), true);
	volatile uint64 slot = 0;
	syncobj->PublishTo(&slot);
	syncobj->Signal(3);
	CHECK_EQ(slot, 3);
	syncobj->Reset();
	CHECK_EQ(slot, 0);

	// a binary syncobj: a new fence makes it unsignaled again
	syncobj->Signal(0);
	BReference<Fence> fence = NewFence();
	syncobj->InsertFence(0, fence);
	CHECK_EQ(slot, 0);
	fence->Signal();

	// unpublished: cleared, and left alone afterwards
	syncobj->UnpublishFrom(&slot);
	CHECK_EQ(slot, 0);
	slot = 77;
	syncobj->Signal(9);
	CHECK_EQ(slot, 77);
}
