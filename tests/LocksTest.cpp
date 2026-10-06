#include "Test.h"

#include <sched.h>
#include <thread>
#include <vector>

#include "Locks/ConditionVariable.h"
#include "Locks/Mutex.h"
#include "Locks/RecursiveLock.h"


static void
RunThreads(int count, void (*function)(int index, void *data), void *data)
{
	std::vector<std::thread> threads;
	for (int i = 0; i < count; i++)
		threads.emplace_back(function, i, data);
	for (std::thread &thread : threads)
		thread.join();
}


TEST(Mutex, AcquireRelease)
{
	Mutex mutex;
	CHECK_EQ(mutex.Acquire(), B_OK);
	mutex.Release();
	CHECK_EQ(mutex.Acquire(), B_OK);
	mutex.Release();
	CHECK_EQ(mutex.fValue, 0);
}


TEST(Mutex, TimeoutWhenHeld)
{
	Mutex mutex(true);
	status_t result = B_OK;
	std::thread thread([&] {
		result = mutex.Acquire(B_RELATIVE_TIMEOUT, 20000);
	});
	thread.join();
	CHECK_EQ(result, B_TIMED_OUT);
	mutex.Release();
	CHECK_EQ(mutex.Acquire(B_RELATIVE_TIMEOUT, 20000), B_OK);
	mutex.Release();
}


struct CounterData {
	Mutex mutex;
	volatile int64 counter = 0;
	int32 iterations;
};


static void
CountUnderMutex(int index, void *_data)
{
	CounterData &data = *(CounterData*)_data;
	for (int32 i = 0; i < data.iterations; i++) {
		data.mutex.Acquire();
		int64 value = data.counter;
		if ((i + index) % 64 == 0)
			sched_yield();
		data.counter = value + 1;
		data.mutex.Release();
	}
}


// Mutual exclusion under contention. Also the regression test for the lost
// wakeup in Acquire() (taking the mutex while _kern_mutex_unblock() handed
// it over to a waiter), which deadlocked here.
TEST(Mutex, Contention)
{
	CounterData data;
	data.iterations = 100000;
	RunThreads(8, CountUnderMutex, &data);
	CHECK_EQ(data.counter, 8 * 100000);
	CHECK_EQ(data.mutex.fValue, 0);
}


static void
HandOff(int index, void *_data)
{
	CounterData &data = *(CounterData*)_data;
	for (int32 i = 0; i < data.iterations; i++) {
		data.mutex.Acquire();
		data.counter++;
		data.mutex.Release();
		// some threads block in the kernel, others retake the mutex at once
		// while a release hands it over to a waiter
		if (index % 2 == 0 && i % 8 == 0)
			snooze(1);
	}
}


TEST(Mutex, HandOff)
{
	CounterData data;
	data.iterations = 200000;
	RunThreads(6, HandOff, &data);
	CHECK_EQ(data.counter, 6 * 200000);
	CHECK_EQ(data.mutex.fValue, 0);
}


TEST(RecursiveLock, Nested)
{
	RecursiveLock lock;
	CHECK_EQ(lock.Acquire(), B_OK);
	CHECK_EQ(lock.Acquire(), B_OK);
	CHECK_EQ(lock.Acquire(), B_OK);
	CHECK_EQ(lock.LockCount(), 3);
	CHECK_EQ(lock.Holder(), find_thread(NULL));

	status_t result = B_OK;
	std::thread other([&] {
		result = lock.Acquire(B_RELATIVE_TIMEOUT, 20000);
	});
	other.join();
	CHECK_EQ(result, B_TIMED_OUT);

	lock.Release();
	lock.Release();
	CHECK_EQ(lock.LockCount(), 1);
	lock.Release();
	CHECK_EQ(lock.LockCount(), 0);
	CHECK_EQ(lock.Holder(), -1);

	std::thread other2([&] {
		result = lock.Acquire(B_RELATIVE_TIMEOUT, 1000000);
		if (result == B_OK)
			lock.Release();
	});
	other2.join();
	CHECK_EQ(result, B_OK);
}


struct QueueData {
	RecursiveLock lock;
	ConditionVariable condition;
	int32 produced = 0;
	int32 consumed = 0;
	int32 total;
};


static void
ProduceConsume(int index, void *_data)
{
	QueueData &data = *(QueueData*)_data;
	if (index == 0) {
		for (int32 i = 0; i < data.total; i++) {
			data.lock.Acquire();
			data.produced++;
			data.condition.Release();
			data.lock.Release();
			if (i % 16 == 0)
				sched_yield();
		}
		return;
	}
	data.lock.Acquire();
	while (data.consumed < data.total) {
		while (data.consumed == data.produced)
			data.condition.Acquire(data.lock);
		data.consumed++;
	}
	data.lock.Release();
}


// a lost condition variable wakeup hangs here
TEST(ConditionVariable, ProducerConsumer)
{
	QueueData data;
	data.total = 50000;
	RunThreads(2, ProduceConsume, &data);
	CHECK_EQ(data.consumed, data.total);
}
