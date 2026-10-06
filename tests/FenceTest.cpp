#include "Test.h"

#include <atomic>
#include <thread>
#include <vector>

#include "Fence.h"
#include "FenceGroup.h"


class CountHandler: public Fence::Handler {
public:
	std::atomic<int32> count{0};

	void Do(Fence *fence) override
	{
		(void)fence;
		count++;
	}
};


static BReference<Fence>
NewFence()
{
	return BReference<Fence>(new Fence(), true);
}


TEST(Fence, SignalRunsHandlers)
{
	BReference<Fence> fence = NewFence();
	CountHandler a, b;
	fence->OnSignal(&a);
	fence->OnSignal(&b);
	CHECK(!fence->IsSignaled());
	CHECK_EQ(a.count, 0);
	fence->Signal();
	CHECK(fence->IsSignaled());
	CHECK_EQ(a.count, 1);
	CHECK_EQ(b.count, 1);
	fence->Signal();
	CHECK_EQ(a.count, 1);
	CHECK_EQ(b.count, 1);
}


TEST(Fence, OnSignalAfterSignalRunsAtOnce)
{
	BReference<Fence> fence = NewFence();
	fence->Signal();
	CountHandler handler;
	fence->OnSignal(&handler);
	CHECK_EQ(handler.count, 1);
	// not registered, so cancelling does nothing
	fence->OnSignalCancel(&handler);
}


TEST(Fence, Cancel)
{
	BReference<Fence> fence = NewFence();
	CountHandler a, b;
	fence->OnSignal(&a);
	fence->OnSignal(&b);
	fence->OnSignalCancel(&a);
	fence->OnSignalCancel(&a);
	fence->Signal();
	CHECK_EQ(a.count, 0);
	CHECK_EQ(b.count, 1);
}


TEST(Fence, SignaledFenceInstance)
{
	BReference<SignaledFence> fence = SignaledFence::Instance();
	CHECK(fence->IsSignaled());
	CHECK(fence.Get() == SignaledFence::Instance().Get());
}


TEST(Fence, WaitNonDomain)
{
	BReference<Fence> fence = NewFence();
	std::thread signaler([&] {
		snooze(20000);
		fence->Signal();
	});
	CHECK_EQ(fence->WaitNonDomain(), B_OK);
	signaler.join();
	CHECK_EQ(fence->WaitNonDomain(), B_OK);
}


TEST(Fence, WaitNonDomainTimeout)
{
	BReference<Fence> fence = NewFence();
	CHECK_EQ(fence->WaitNonDomain(B_RELATIVE_TIMEOUT, 20000), B_TIMED_OUT);
	// the waiter's handler is gone; signaling must not run it
	fence->Signal();
}


TEST(Fence, WaitMultipleAny)
{
	BReference<Fence> fences[3] = {NewFence(), NewFence(), NewFence()};
	uint32 first = 0xffffffff;
	CHECK_EQ(Fence::WaitMultiple(&first, fences, 3, {}, 0), B_TIMED_OUT);

	std::thread signaler([&] {
		snooze(20000);
		fences[2]->Signal();
	});
	CHECK_EQ(Fence::WaitMultiple(&first, fences, 3), B_OK);
	signaler.join();
	CHECK_EQ(first, 2);
}


TEST(Fence, WaitMultipleAll)
{
	BReference<Fence> fences[3] = {NewFence(), NewFence(), NewFence()};
	fences[1]->Signal();
	std::thread signaler([&] {
		snooze(10000);
		fences[0]->Signal();
		snooze(10000);
		fences[2]->Signal();
	});
	CHECK_EQ(Fence::WaitMultiple(NULL, fences, 3, {.all = true}), B_OK);
	signaler.join();
	CHECK(fences[0]->IsSignaled() && fences[2]->IsSignaled());
}


// Every handler registered while another thread signals runs exactly once.
TEST(Fence, OnSignalRacesSignal)
{
	for (int round = 0; round < 2000; round++) {
		BReference<Fence> fence = NewFence();
		CountHandler handlers[8];
		std::atomic<bool> go{false};
		std::thread registerer([&] {
			while (!go) {}
			for (CountHandler &handler : handlers)
				fence->OnSignal(&handler);
		});
		go = true;
		fence->Signal();
		registerer.join();
		for (CountHandler &handler : handlers)
			CHECK_EQ(handler.count, 1);
	}
}


class LockingHandler: public Fence::Handler {
public:
	RecursiveLock *lock;
	std::atomic<int32> count{0};

	void Do(Fence *fence) override
	{
		(void)fence;
		lock->Acquire();
		count++;
		lock->Release();
	}
};


class SlowHandler: public Fence::Handler {
public:
	void Do(Fence *fence) override
	{
		(void)fence;
		snooze(1000);
	}
};


// Regression test: OnSignal() ran the handler of an already signaled fence
// under the fence's lock. The handler takes another lock (a CS takes the
// ring's domain) that the signaling thread (the interrupt thread) holds
// while it waits for the fence's lock: deadlock, seen under glmark2.
// The slow handler keeps the signaling thread between two handlers while
// the fence is already signaled.
TEST(Fence, OnSignalLockOrder)
{
	RecursiveLock other;
	for (int round = 0; round < 200; round++) {
		BReference<Fence> fence = NewFence();
		SlowHandler slow;
		fence->OnSignal(&slow);
		LockingHandler handler;
		handler.lock = &other;
		std::thread signaler([&] {
			other.Acquire();
			fence->Signal();
			other.Release();
		});
		while (!fence->IsSignaled()) {}
		fence->OnSignal(&handler);
		signaler.join();
		CHECK_EQ(handler.count, 1);
	}
}


TEST(FenceGroup, All)
{
	BReference<Fence> fences[3] = {NewFence(), NewFence(), NewFence()};
	BReference<Fence> group(new FenceGroup(fences, 3, {.all = true}), true);
	fences[0]->Signal();
	fences[2]->Signal();
	CHECK(!group->IsSignaled());
	fences[1]->Signal();
	CHECK(group->IsSignaled());
}


TEST(FenceGroup, Any)
{
	BReference<Fence> fences[3] = {NewFence(), NewFence(), NewFence()};
	BReference<Fence> group(new FenceGroup(fences, 3), true);
	CountHandler handler;
	group->OnSignal(&handler);
	fences[1]->Signal();
	CHECK(group->IsSignaled());
	fences[0]->Signal();
	fences[2]->Signal();
	CHECK_EQ(handler.count, 1);
}


TEST(FenceGroup, Nested)
{
	BReference<Fence> fences[3] = {NewFence(), NewFence(), NewFence()};
	BReference<Fence> inner[2] = {
		BReference<Fence>(new FenceGroup(fences, 2, {.all = true}), true),
		fences[2]
	};
	BReference<Fence> group(new FenceGroup(inner, 2, {.all = true}), true);
	// the outer group waits for the inner group's fences itself
	inner[0].Unset();
	fences[0]->Signal();
	fences[1]->Signal();
	CHECK(!group->IsSignaled());
	fences[2]->Signal();
	CHECK(group->IsSignaled());
}


TEST(FenceGroup, AllWithSignaledFences)
{
	BReference<Fence> fences[2] = {NewFence(), NewFence()};
	fences[0]->Signal();
	BReference<Fence> group(new FenceGroup(fences, 2, {.all = true}), true);
	CHECK(!group->IsSignaled());
	fences[1]->Signal();
	CHECK(group->IsSignaled());

	BReference<Fence> group2(new FenceGroup(fences, 2, {.all = true}), true);
	CHECK(group2->IsSignaled());
}


TEST(FenceGroup, AnyWithSignaledFirstFence)
{
	BReference<Fence> fences[2] = {NewFence(), NewFence()};
	fences[0]->Signal();
	BReference<Fence> group(new FenceGroup(fences, 2), true);
	CHECK(group->IsSignaled());
	fences[1]->Signal();
}


// Regression test: the interrupt thread signaled a fence while the group
// waiting for it lost its last reference in another thread
// (use-after-free of the group, fixed with Handler::Retain()).
TEST(FenceGroup, ReleasedWhileSignaling)
{
	for (int round = 0; round < 5000; round++) {
		BReference<Fence> fences[2] = {NewFence(), NewFence()};
		FenceGroup *group = new FenceGroup(fences, 2, {.all = (round & 1) != 0});
		std::atomic<bool> go{false};
		std::thread signaler([&] {
			while (!go) {}
			fences[0]->Signal();
			fences[1]->Signal();
		});
		go = true;
		group->ReleaseReference();
		signaler.join();
	}
}


class FlagHandler: public Fence::Handler {
public:
	std::atomic<bool> started{false};
	std::atomic<bool> proceed{false};

	void Do(Fence *fence) override
	{
		(void)fence;
		started = true;
		while (!proceed)
			snooze(100);
	}
};


// The same deterministically: the group loses its last reference while
// the signaling thread is inside the group's Signal(). Needs the guarded
// heap to see the use-after-free (run-unit-tests.sh does that).
TEST(FenceGroup, ReleasedInHandler)
{
	BReference<Fence> fence = NewFence();
	FenceGroup *group = new FenceGroup(&fence, 1);
	FlagHandler handler;
	group->OnSignal(&handler);
	std::thread signaler([&] {
		fence->Signal();
	});
	while (!handler.started)
		snooze(100);
	group->ReleaseReference();
	handler.proceed = true;
	signaler.join();
	CHECK(fence->IsSignaled());
}
