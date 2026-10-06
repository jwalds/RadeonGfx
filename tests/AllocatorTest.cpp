#include "Test.h"

#include <stdlib.h>
#include <vector>

#include "ExternalAllocator.h"


TEST(ExternalAllocator, AllocFree)
{
	ExternalAllocator allocator;
	allocator.Register(0x1000, 0x10000);
	CHECK_EQ(allocator.TotalSize(), 0x10000);

	uint64_t a = 0, b = 0;
	REQUIRE(allocator.Alloc(a, 0x100));
	REQUIRE(allocator.Alloc(b, 0x200));
	CHECK_EQ(a, 0x1000);
	CHECK_EQ(b, 0x1100);
	CHECK_EQ(allocator.AllocSize(), 0x300);

	allocator.Free(a);
	allocator.Free(b);
	CHECK_EQ(allocator.AllocSize(), 0);

	uint64_t largest;
	uint32_t count;
	allocator.GetFreeStats(largest, count);
	CHECK_EQ(largest, 0x10000);
	CHECK_EQ(count, 1);
}


TEST(ExternalAllocator, Exhaustion)
{
	ExternalAllocator allocator;
	allocator.Register(0, 0x4000);
	uint64_t a, b;
	REQUIRE(allocator.Alloc(a, 0x4000));
	CHECK(!allocator.Alloc(b, 1));
	CHECK(!allocator.AllocAligned(b, 0x1000, 0x1000));
	allocator.Free(a);
	CHECK(allocator.Alloc(b, 0x4000));
	allocator.Free(b);
}


TEST(ExternalAllocator, FreeMerges)
{
	ExternalAllocator allocator;
	allocator.Register(0, 0x3000);
	uint64_t a, b, c;
	REQUIRE(allocator.Alloc(a, 0x1000));
	REQUIRE(allocator.Alloc(b, 0x1000));
	REQUIRE(allocator.Alloc(c, 0x1000));

	uint64_t largest;
	uint32_t count;
	allocator.Free(a);
	allocator.Free(c);
	allocator.GetFreeStats(largest, count);
	CHECK_EQ(count, 2);
	CHECK_EQ(largest, 0x1000);

	allocator.Free(b);
	allocator.GetFreeStats(largest, count);
	CHECK_EQ(count, 1);
	CHECK_EQ(largest, 0x3000);
}


TEST(ExternalAllocator, AllocAt)
{
	ExternalAllocator allocator;
	allocator.Register(0, 0x10000);
	CHECK(allocator.AllocAt(0x4000, 0x1000));
	CHECK(!allocator.AllocAt(0x4800, 0x100));
	CHECK(!allocator.AllocAt(0x3800, 0x1000));
	CHECK(!allocator.AllocAt(0xf000, 0x2000));
	CHECK(allocator.AllocAt(0x3000, 0x1000));
	CHECK(allocator.AllocAt(0x5000, 0x1000));
	CHECK_EQ(allocator.AllocSize(), 0x3000);
	allocator.Free(0x4000);
	allocator.Free(0x3000);
	allocator.Free(0x5000);
	CHECK_EQ(allocator.AllocSize(), 0);
}


TEST(ExternalAllocator, Aligned)
{
	ExternalAllocator allocator;
	allocator.Register(0x1000, 0x100000);
	uint64_t a, b;
	REQUIRE(allocator.Alloc(a, 0x100));
	REQUIRE(allocator.AllocAligned(b, 0x10000, 0x10000));
	CHECK_EQ(b % 0x10000, 0);
	CHECK(b >= 0x1100 && b + 0x10000 <= 0x101000);
	allocator.Free(a);
	allocator.Free(b);
}


// Regression test: AllocAligned() only looked at the smallest free block of
// at least size + align - 1, and failed while a larger block had an aligned
// fit (GTT "exhaustion" with RADV's 16 MB alignment).
TEST(ExternalAllocator, AlignedSearchesAllBlocks)
{
	ExternalAllocator allocator;
	allocator.Register(0, 0x100000);
	// free blocks [0x1000, 0x9000), the smallest that is large enough but
	// has no 64 KB aligned start, and [0x20000, 0x100000)
	REQUIRE(allocator.AllocAt(0, 0x1000));
	REQUIRE(allocator.AllocAt(0x9000, 0x17000));
	uint64_t ptr;
	REQUIRE(allocator.AllocAligned(ptr, 0x8000, 0x10000));
	CHECK_EQ(ptr, 0x20000);
	allocator.Free(ptr);

	// a fit that is only aligned inside the larger block
	REQUIRE(allocator.AllocAligned(ptr, 0x8000, 0x8000));
	CHECK_EQ(ptr % 0x8000, 0);
	allocator.Free(ptr);
}


// Random allocations and frees against a page bitmap: no overlaps, correct
// alignment and statistics, and an allocation fails only if there is no
// fit.
TEST(ExternalAllocator, RandomAgainstModel)
{
	const uint64_t kPage = 0x1000, kPages = 512, kBase = 0x10000000;
	ExternalAllocator allocator;
	allocator.Register(kBase, kPages * kPage);

	std::vector<bool> used(kPages, false);
	struct Allocation {
		uint64_t ptr, size;
	};
	std::vector<Allocation> allocations;
	uint64_t usedSize = 0;
	srand(1234);

	auto fits = [&](uint64_t size, uint64_t align) {
		for (uint64_t page = 0; page + size / kPage <= kPages; page++) {
			if ((kBase + page * kPage) % align != 0)
				continue;
			bool free = true;
			for (uint64_t i = 0; i < size / kPage && free; i++)
				free = !used[page + i];
			if (free)
				return true;
		}
		return false;
	};

	for (int step = 0; step < 20000; step++) {
		if (allocations.empty() || rand() % 100 < 55) {
			uint64_t size = (1 + rand() % (rand() % 4 == 0 ? 64 : 8)) * kPage;
			uint64_t align = kPage << (rand() % 7);
			bool aligned = rand() % 2 == 0;
			uint64_t ptr = 0;
			bool ok = aligned ? allocator.AllocAligned(ptr, size, align)
				: allocator.Alloc(ptr, size);
			if (!ok) {
				CHECK(!fits(size, aligned ? align : kPage));
				continue;
			}
			REQUIRE(ptr >= kBase && ptr + size <= kBase + kPages * kPage);
			if (aligned)
				CHECK_EQ(ptr % align, 0);
			uint64_t page = (ptr - kBase) / kPage;
			for (uint64_t i = 0; i < size / kPage; i++) {
				REQUIRE(!used[page + i]);
				used[page + i] = true;
			}
			allocations.push_back({ptr, size});
			usedSize += size;
		} else {
			size_t index = rand() % allocations.size();
			Allocation allocation = allocations[index];
			allocations[index] = allocations.back();
			allocations.pop_back();
			allocator.Free(allocation.ptr);
			uint64_t page = (allocation.ptr - kBase) / kPage;
			for (uint64_t i = 0; i < allocation.size / kPage; i++)
				used[page + i] = false;
			usedSize -= allocation.size;
		}
		REQUIRE(allocator.AllocSize() == usedSize);
	}

	for (Allocation &allocation : allocations)
		allocator.Free(allocation.ptr);
	uint64_t largest;
	uint32_t count;
	allocator.GetFreeStats(largest, count);
	CHECK_EQ(count, 1);
	CHECK_EQ(largest, kPages * kPage);
}
