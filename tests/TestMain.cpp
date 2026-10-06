#include "Test.h"

#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>


static TestCase *sFirstTest = NULL;
static TestCase **sLastTest = &sFirstTest;
static TestCase *volatile sCurrentTest = NULL;
static bigtime_t volatile sTestStart = 0;
static int32 sFailures = 0;
static bigtime_t sTimeout = 30000000;


void
RegisterTest(TestCase *test)
{
	// in registration order, which is source order within a file
	*sLastTest = test;
	sLastTest = &test->next;
}


void
TestFailed(const char *file, int line, const char *expression)
{
	atomic_add(&sFailures, 1);
	printf("  %s:%d: CHECK(%s) failed\n", file, line, expression);
}


void
TestFailedValues(const char *file, int line, const char *expression,
	unsigned long long a, unsigned long long b)
{
	atomic_add(&sFailures, 1);
	printf("  %s:%d: CHECK_EQ(%s) failed: %#llx != %#llx\n", file, line,
		expression, a, b);
}


static void
Report(const char *what)
{
	TestCase *test = sCurrentTest;
	// async-signal context: write() only
	char buffer[256];
	int length = snprintf(buffer, sizeof(buffer), "[%s] %s.%s\n", what,
		test != NULL ? test->suite : "?", test != NULL ? test->name : "?");
	write(STDOUT_FILENO, buffer, length);
}


static void
CrashHandler(int signal)
{
	Report(signal == SIGABRT ? "ABORT" : "CRASH");
	_exit(4);
}


static status_t
Watchdog(void *)
{
	for (;;) {
		snooze(100000);
		if (sCurrentTest != NULL && system_time() - sTestStart > sTimeout) {
			Report("HANG");
			_exit(3);
		}
	}
	return B_OK;
}


static bool
Selected(TestCase *test, int argc, char **argv)
{
	if (argc < 2)
		return true;
	for (int i = 1; i < argc; i++) {
		size_t length = strlen(test->suite);
		if (strcmp(argv[i], test->suite) == 0)
			return true;
		if (strncmp(argv[i], test->suite, length) == 0 && argv[i][length] == '.'
			&& strcmp(argv[i] + length + 1, test->name) == 0)
			return true;
	}
	return false;
}


int
main(int argc, char **argv)
{
	// usage: unit_tests [suite | suite.name]...
	setvbuf(stdout, NULL, _IOLBF, 0);
	disable_debugger(true);
	signal(SIGSEGV, CrashHandler);
	signal(SIGBUS, CrashHandler);
	signal(SIGILL, CrashHandler);
	signal(SIGFPE, CrashHandler);
	signal(SIGABRT, CrashHandler);

	const char *timeout = getenv("TEST_TIMEOUT");
	if (timeout != NULL)
		sTimeout = atoll(timeout) * 1000000;

	resume_thread(spawn_thread(Watchdog, "test watchdog", B_NORMAL_PRIORITY,
		NULL));

	int32 count = 0, failed = 0;
	for (TestCase *test = sFirstTest; test != NULL; test = test->next) {
		if (!Selected(test, argc, argv))
			continue;
		int32 failures = sFailures;
		printf("[ RUN  ] %s.%s\n", test->suite, test->name);
		sTestStart = system_time();
		sCurrentTest = test;
		test->function();
		sCurrentTest = NULL;
		bigtime_t duration = system_time() - sTestStart;
		bool ok = sFailures == failures;
		printf("[ %s ] %s.%s (%" B_PRId64 " ms)\n", ok ? " OK " : "FAIL",
			test->suite, test->name, duration / 1000);
		count++;
		if (!ok)
			failed++;
	}

	printf("%" B_PRId32 " tests, %" B_PRId32 " failed\n", count, failed);
	return failed == 0 && count > 0 ? 0 : 1;
}
