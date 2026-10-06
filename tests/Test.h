#pragma once

// A small unit test harness: TEST(Suite, Name) { CHECK(...); } registers a
// test; tests/TestMain.cpp runs them with a per-test watchdog, so a deadlock
// or a crash is reported with the test's name.

#include <OS.h>
#include <stdio.h>


struct TestCase {
	const char *suite;
	const char *name;
	void (*function)();
	TestCase *next;
};

void RegisterTest(TestCase *test);
void TestFailed(const char *file, int line, const char *expression);
void TestFailedValues(const char *file, int line, const char *expression,
	unsigned long long a, unsigned long long b);


#define TEST(suite, name) \
	static void suite##_##name(); \
	static TestCase suite##_##name##_case \
		= {#suite, #name, suite##_##name, NULL}; \
	static int suite##_##name##_registered \
		= (RegisterTest(&suite##_##name##_case), 0); \
	static void suite##_##name()

#define CHECK(expression) \
	do { \
		if (!(expression)) \
			TestFailed(__FILE__, __LINE__, #expression); \
	} while (0)

#define CHECK_EQ(a, b) \
	do { \
		unsigned long long _a = (unsigned long long)(a); \
		unsigned long long _b = (unsigned long long)(b); \
		if (_a != _b) \
			TestFailedValues(__FILE__, __LINE__, #a " == " #b, _a, _b); \
	} while (0)

// stops the test on failure
#define REQUIRE(expression) \
	do { \
		if (!(expression)) { \
			TestFailed(__FILE__, __LINE__, #expression); \
			return; \
		} \
	} while (0)
