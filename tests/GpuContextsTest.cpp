#include "Test.h"

#include "GpuContexts.h"


TEST(GpuContexts, CreateDelete)
{
	GpuContexts contexts;
	uint32 a = contexts.Create(100);
	uint32 b = contexts.Create(100);
	uint32 c = contexts.Create(200);
	CHECK(a != b && b != c);
	CHECK(contexts.Exists(100, a));
	CHECK(!contexts.Exists(200, a));
	CHECK_EQ(contexts.Delete(100, a), B_OK);
	CHECK_EQ(contexts.Delete(100, a), B_BAD_VALUE);
	CHECK(!contexts.Exists(100, a));
	contexts.DeleteTeam(100);
	CHECK(!contexts.Exists(100, b));
	CHECK(contexts.Exists(200, c));
	CHECK_EQ(contexts.Count(), 1);
}


TEST(GpuContexts, ResetMarksExistingContexts)
{
	GpuContexts contexts;
	uint32 guilty = contexts.Create(100);
	uint32 innocent = contexts.Create(200);
	bool reset, isGuilty;
	REQUIRE(contexts.Query(100, guilty, reset, isGuilty) == B_OK);
	CHECK(!reset && !isGuilty);

	contexts.Reset(100, guilty);
	CHECK_EQ(contexts.ResetCount(), 1);
	REQUIRE(contexts.Query(100, guilty, reset, isGuilty) == B_OK);
	CHECK(reset && isGuilty);
	CHECK(contexts.IsGuilty(100, guilty));
	REQUIRE(contexts.Query(200, innocent, reset, isGuilty) == B_OK);
	CHECK(reset && !isGuilty);
	CHECK(!contexts.IsGuilty(200, innocent));

	// created after the reset: not affected
	uint32 fresh = contexts.Create(100);
	REQUIRE(contexts.Query(100, fresh, reset, isGuilty) == B_OK);
	CHECK(!reset && !isGuilty);

	// a reset without a known culprit
	contexts.Reset(-1, 0);
	REQUIRE(contexts.Query(100, fresh, reset, isGuilty) == B_OK);
	CHECK(reset && !isGuilty);

	bool dummy;
	CHECK_EQ(contexts.Query(300, 1, dummy, dummy), B_BAD_VALUE);
}


TEST(GpuContexts, SharedCounter)
{
	GpuContexts contexts;
	REQUIRE(contexts.InitSharedCounter("GpuContexts test counter") == B_OK);
	area_id area = find_area("GpuContexts test counter");
	REQUIRE(area >= B_OK);
	area_info info;
	REQUIRE(get_area_info(area, &info) == B_OK);
	const volatile int32 *counter = (const volatile int32*)info.address;
	CHECK_EQ(*counter, 0);
	contexts.Reset(-1, 0);
	CHECK_EQ(*counter, 1);
}
