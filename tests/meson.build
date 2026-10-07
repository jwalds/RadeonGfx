# Host unit tests, no GPU needed: meson test -C <build dir>
unit_tests = executable('unit_tests',
	[
		'TestMain.cpp',
		'LocksTest.cpp',
		'FenceTest.cpp',
		'SyncobjTest.cpp',
		'AllocatorTest.cpp',
		'GpuContextsTest.cpp',
		'../ExternalAllocator.cpp',
		'../FdObject.cpp',
		'../GpuContexts.cpp',
		'../util/AVLTreeBase.cpp',
	],
	include_directories: [
		'..',
		'../headers',
		'/boot/system/develop/headers/private/shared',
		'/boot/system/develop/headers/private/system',
		'/boot/system/develop/headers/private/system/arch/x86_64',
	],
	dependencies: [
		dep_libsync,
		dep_libbe,
		Locks,
		SADomains,
	],
	install: false
)

foreach suite : ['Mutex', 'RecursiveLock', 'ConditionVariable', 'Fence',
		'FenceGroup', 'Syncobj', 'ExternalAllocator', 'GpuContexts']
	# the guarded heap turns a use-after-free into a crash
	test(suite, unit_tests, args: [suite], timeout: 120,
		env: ['TEST_TIMEOUT=60', 'MALLOC_DEBUG=g',
			'LD_PRELOAD=/boot/system/lib/libroot_debug.so'])
endforeach
