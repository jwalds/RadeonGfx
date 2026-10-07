#include "RadeonGfxAccelerant.h"

#include "../RadeonServer.h"
#include <Autolock.h>
#include <Messenger.h>
#define _DEFAULT_SOURCE
extern "C" {
#include <xf86drm.h>
#include <libdrm/drm.h>
#include <libdrm/amdgpu_drm.h>
#include <libdrm/amdgpu.h>
}
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <new>

//#define CheckRet(err) {status_t _err = (err); if (_err < B_OK) return _err;}

#define CheckRet(err) { \
	status_t _err = (err); \
	if (_err < B_OK) { \
		fprintf(stderr, "[!] %s:%d: error %#x (%s)\n", __FILE__, __LINE__, _err, strerror(_err)); \
		return _err; \
	} \
} \


// RADEONGFX_STATS=1: how often each call to the server was made and how
// long it took, printed when the process exits (per-frame overhead)
enum {
	kDrmMmapIndex,
	kDrmIoctlIndex,
	kDrmVersionIndex,
	kDrmCloseBufferHandleIndex,
	kDrmPrimeHandleToFDIndex,
	kDrmPrimeFDToHandleIndex,
	kDrmSyncobjCreateIndex,
	kDrmSyncobjDestroyIndex,
	kDrmSyncobjHandleToFDIndex,
	kDrmSyncobjFDToHandleIndex,
	kDrmSyncobjImportSyncFileIndex,
	kDrmSyncobjExportSyncFileIndex,
	kDrmSyncobjWaitIndex,
	kDrmSyncobjResetIndex,
	kDrmSyncobjSignalIndex,
	kDrmSyncobjTimelineSignalIndex,
	kDrmSyncobjTimelineWaitIndex,
	kDrmSyncobjQueryIndex,
	kDrmSyncobjTransferIndex,
	kDrmSyncobjAccumulateIndex,
	kAmdgpuQueryInfoIndex,
	kAmdgpuBoAllocIndex,
	kAmdgpuCreateBoFromUserMemIndex,
	kAmdgpuBoQueryInfoIndex,
	kAmdgpuBoSetMetadataIndex,
	kAmdgpuBoVaOpRawIndex,
	kAmdgpuBoCpuMapIndex,
	kAmdgpuCsSubmitRawIndex,
	kAmdgpuWaitCsIndex,
	kAmdgpuCtxRawIndex,
	kDisplayGetConsumerIndex,
	kDisplayUpdateCursorIndex,
	kCallCount
};

static const char *kCallNames[] = {
	"DrmMmap",
	"DrmIoctl",
	"DrmVersion",
	"DrmCloseBufferHandle",
	"DrmPrimeHandleToFD",
	"DrmPrimeFDToHandle",
	"DrmSyncobjCreate",
	"DrmSyncobjDestroy",
	"DrmSyncobjHandleToFD",
	"DrmSyncobjFDToHandle",
	"DrmSyncobjImportSyncFile",
	"DrmSyncobjExportSyncFile",
	"DrmSyncobjWait",
	"DrmSyncobjReset",
	"DrmSyncobjSignal",
	"DrmSyncobjTimelineSignal",
	"DrmSyncobjTimelineWait",
	"DrmSyncobjQuery",
	"DrmSyncobjTransfer",
	"DrmSyncobjAccumulate",
	"AmdgpuQueryInfo",
	"AmdgpuBoAlloc",
	"AmdgpuCreateBoFromUserMem",
	"AmdgpuBoQueryInfo",
	"AmdgpuBoSetMetadata",
	"AmdgpuBoVaOpRaw",
	"AmdgpuBoCpuMap",
	"AmdgpuCsSubmitRaw",
	"AmdgpuWaitCs",
	"AmdgpuCtxRaw",
	"DisplayGetConsumer",
	"DisplayUpdateCursor",
};

static struct CallStats {
	bool enabled = getenv("RADEONGFX_STATS") != NULL;
	int64 count[kCallCount] = {};
	bigtime_t time[kCallCount] = {};
	bigtime_t start = system_time();

	~CallStats()
	{
		if (!enabled)
			return;
		bigtime_t elapsed = system_time() - start;
		fprintf(stderr, "RADEONGFX_STATS: %" B_PRId64 " ms\n", elapsed / 1000);
		for (int i = 0; i < kCallCount; i++) {
			if (count[i] == 0)
				continue;
			fprintf(stderr, "  %-28s %9" B_PRId64 " calls %8.1f/s %7.1f us"
				" %7.1f%% of the time\n", kCallNames[i], count[i],
				count[i] * 1e6 / elapsed, (double)time[i] / count[i],
				100.0 * time[i] / elapsed);
		}
	}
} sCallStats;


class CallTimer {
public:
	CallTimer(int index): fIndex(index),
		fStart(sCallStats.enabled ? system_time() : 0) {}
	~CallTimer()
	{
		if (!sCallStats.enabled)
			return;
		atomic_add64(&sCallStats.count[fIndex], 1);
		atomic_add64(&sCallStats.time[fIndex], system_time() - fStart);
	}

private:
	int fIndex;
	bigtime_t fStart;
};


BLocker RadeonGfxAccelerant::sContextLock("radeon_gfx contexts");
std::map<uint32, int32> RadeonGfxAccelerant::sContextResetCounters;


RadeonGfxAccelerant::RadeonGfxAccelerant(int fd)
{
	fFd.SetTo(dup(fd));
	fcntl(fFd.Get(), F_SETFD, FD_CLOEXEC);
	fConn.SetMessenger(BMessenger(RADEON_GFX_SERVER_SIGNATURE));

	area_id area = find_area(RADEON_GFX_RESET_COUNTER_AREA);
	if (area >= B_OK) {
		void *address;
		// not the server's name: find_area() must not find a client's clone
		fResetCounterArea.SetTo(clone_area("radeon_gfx reset counter clone",
			&address, B_ANY_ADDRESS, B_READ_AREA, area));
		if (fResetCounterArea.IsSet())
			fResetCounter = (const volatile int32*)address;
	}
}

status_t RadeonGfxAccelerant::InitCheck()
{
	if (!fFd.IsSet() || !fConn.Messenger().IsValid()) return B_ERROR;
	return B_OK;
}

void *RadeonGfxAccelerant::QueryInterface(const char *iface, uint32 version)
{
	if (strcmp(iface, B_ACCELERANT_IFACE_DRM) == 0 && version <= 0) {
		return static_cast<AccelerantDrm*>(this);
	}
	if (strcmp(iface, B_ACCELERANT_IFACE_AMDGPU) == 0 && version <= 0) {
		return static_cast<AccelerantAmdgpu*>(this);
	}
#if 0
	if (strcmp(iface, B_ACCELERANT_IFACE_DISPLAY) == 0 && version <= 0) {
		return static_cast<AccelerantDisplay*>(this);
	}
#endif
	return Accelerant::QueryInterface(iface, version);
}


// #pragma mark - DRM

/*!	The server's CPU visible VRAM (256 MB) is cloned once per process and
	kept: cloning it maps every page (about 6 ms), it was done for every CPU
	map of a VRAM buffer. libdrm's amdgpu_bo_cpu_unmap() deletes the clone of
	a map, except this one (by its name).
*/
uint8 *RadeonGfxAccelerant::SharedClone(area_id area)
{
	static area_id sSource = -1;
	static uint8 *sAddress = NULL;
	BAutolock lock(sContextLock);
	if (sAddress != NULL && sSource == area)
		return sAddress;
	if (sAddress != NULL) {
		// a different area (the server restarted?): not expected
		fprintf(stderr, "[!] radeon_gfx: second shared area %" B_PRId32
			" (have %" B_PRId32 ")\n", area, sSource);
		return NULL;
	}
	void *address;
	area_id clone = clone_area(RADEON_GFX_VRAM_CLONE_NAME, &address,
		B_ANY_ADDRESS, B_READ_AREA | B_WRITE_AREA | B_CLONEABLE_AREA, area);
	if (clone < B_OK)
		return NULL;
	sSource = area;
	sAddress = (uint8*)address;
	return sAddress;
}

void *RadeonGfxAccelerant::DrmMmap(void *addr, size_t length, int prot, int flags, off_t offset)
{
	CallTimer callTimer(kDrmMmapIndex);
	(void)addr;
	(void)length;
	(void)prot;
	(void)flags;

	ThreadLinkHolder link(fConn);
	link.StartMessage(radeonMmapMsg);
	link.Attach<int32>((int32)offset);
	int32 reply;
	link.FlushWithReply(reply);
	if (reply < B_OK) return NULL;
	area_id area;
	uint64 areaOfs;
	link.Read<int32>(&area);
	link.Read<uint64>(&areaOfs);

	area_info info;
	if (get_area_info(area, &info) < B_OK)
		return NULL;

	uint8* adr{};
	if (info.team == B_SYSTEM_TEAM && (info.protection & (B_READ_AREA | B_WRITE_AREA)) != 0) {
		adr = (uint8*)info.address;
	} else if (info.size >= kSharedCloneMinSize) {
		adr = SharedClone(area);
		if (adr == NULL) return NULL;
	} else {
		AreaDeleter mappedArea(clone_area("cloned buffer", (void**)&adr, B_ANY_ADDRESS, B_READ_AREA | B_WRITE_AREA | B_CLONEABLE_AREA, area));
		if (!mappedArea.IsSet()) return NULL;
		mappedArea.Detach();
	}

	return adr + areaOfs;
}

int RadeonGfxAccelerant::DrmIoctl(uint32 request, void *arg)
{
	CallTimer callTimer(kDrmIoctlIndex);
	(void)request;
	(void)arg;
	return ENOSYS;
}


int RadeonGfxAccelerant::DrmVersion(struct drm_version *version)
{
	CallTimer callTimer(kDrmVersionIndex);
	const char *name = "amdgpu";
	const char *date = "20150101";
	const char *desc = "AMD GPU";
	version->version_major = 3;
	version->version_minor = 42;
	version->version_patchlevel = 0;
	version->name_len = strlen(name);
	if (version->name != NULL) strcpy(version->name, name);
	version->date_len = strlen(date);
	if (version->date != NULL) strcpy(version->date, date);
	version->desc_len = strlen(desc);
	if (version->desc != NULL) strcpy(version->desc, desc);
	return B_OK;
}


int RadeonGfxAccelerant::DrmCloseBufferHandle(uint32_t handle)
{
	CallTimer callTimer(kDrmCloseBufferHandleIndex);
	ThreadLinkHolder link(fConn);
	link.StartMessage(radeonIoctlMsg);
	link.Attach<int>(fFd.Get());
	link.Attach<uint32_t>(DRM_IOCTL_GEM_CLOSE);
	link.Attach<uint32_t>(handle);
	link.Attach<uint32_t>(0); // pad
	status_t reply;
	link.FlushWithReply(reply);
	CheckRet(reply);
	return B_OK;
}

int RadeonGfxAccelerant::DrmPrimeHandleToFD(uint32_t handle, uint32_t flags, int *prime_fd)
{
	CallTimer callTimer(kDrmPrimeHandleToFDIndex);
	(void)flags;
	ThreadLinkHolder link(fConn);
	link.StartMessage(radeonIoctlMsg);
	link.Attach<int>(fFd.Get());
	link.Attach<uint32_t>(DRM_IOCTL_PRIME_HANDLE_TO_FD);
	link.Attach<uint32_t>(handle);
	status_t reply;
	link.FlushWithReply(reply);
	link.Read(prime_fd);
	CheckRet(reply);
	return B_OK;
}

int RadeonGfxAccelerant::DrmPrimeFDToHandle(int prime_fd, uint32_t *handle)
{
	CallTimer callTimer(kDrmPrimeFDToHandleIndex);
	ThreadLinkHolder link(fConn);
	link.StartMessage(radeonIoctlMsg);
	link.Attach<int>(fFd.Get());
	link.Attach<uint32_t>(DRM_IOCTL_PRIME_FD_TO_HANDLE);
	link.Attach<int>(prime_fd);
	status_t reply;
	link.FlushWithReply(reply);
	link.Read(handle);
	CheckRet(reply);
	return B_OK;
}


int RadeonGfxAccelerant::DrmSyncobjCreate(uint32_t flags, uint32_t *handle)
{
	CallTimer callTimer(kDrmSyncobjCreateIndex);
	ThreadLinkHolder link(fConn);
	link.StartMessage(radeonIoctlMsg);
	link.Attach<int>(fFd.Get());
	link.Attach<uint32_t>(DRM_IOCTL_SYNCOBJ_CREATE);
	link.Attach<uint32_t>(flags);
	status_t reply;
	link.FlushWithReply(reply);
	link.Read(handle);
	CheckRet(reply);
	return B_OK;
}

int RadeonGfxAccelerant::DrmSyncobjDestroy(uint32_t handle)
{
	CallTimer callTimer(kDrmSyncobjDestroyIndex);
	ThreadLinkHolder link(fConn);
	link.StartMessage(radeonIoctlMsg);
	link.Attach<int>(fFd.Get());
	link.Attach<uint32_t>(DRM_IOCTL_SYNCOBJ_DESTROY);
	drm_syncobj_destroy args {.handle = handle};
	link.Attach(args);
	status_t reply;
	link.FlushWithReply(reply);
	CheckRet(reply);
	return B_OK;
}

int RadeonGfxAccelerant::DrmSyncobjHandleToFD(uint32_t handle, int *obj_fd)
{
	CallTimer callTimer(kDrmSyncobjHandleToFDIndex);
	ThreadLinkHolder link(fConn);
	link.StartMessage(radeonIoctlMsg);
	link.Attach<int>(fFd.Get());
	link.Attach<uint32_t>(DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD);
	link.Attach<uint32_t>(handle);
	status_t reply;
	link.FlushWithReply(reply);
	link.Read(obj_fd);
	CheckRet(reply);
	return B_OK;
}

int RadeonGfxAccelerant::DrmSyncobjFDToHandle(int obj_fd, uint32_t *handle)
{
	CallTimer callTimer(kDrmSyncobjFDToHandleIndex);
	ThreadLinkHolder link(fConn);
	link.StartMessage(radeonIoctlMsg);
	link.Attach<int>(fFd.Get());
	link.Attach<uint32_t>(DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE);
	link.Attach<int>(obj_fd);
	status_t reply;
	link.FlushWithReply(reply);
	link.Read(handle);
	CheckRet(reply);
	return B_OK;
}

int RadeonGfxAccelerant::DrmSyncobjImportSyncFile(uint32_t handle, int sync_file_fd)
{
	CallTimer callTimer(kDrmSyncobjImportSyncFileIndex);
	(void)handle;
	(void)sync_file_fd;
	CheckRet(ENOSYS);
	return B_OK;
}

int RadeonGfxAccelerant::DrmSyncobjExportSyncFile(uint32_t handle, int *sync_file_fd)
{
	CallTimer callTimer(kDrmSyncobjExportSyncFileIndex);
	(void)handle;
	(void)sync_file_fd;
	CheckRet(ENOSYS);
	return B_OK;
}

int RadeonGfxAccelerant::DrmSyncobjWait(uint32_t *handles, unsigned num_handles, int64_t timeout_nsec, unsigned flags, uint32_t *first_signaled)
{
	CallTimer callTimer(kDrmSyncobjWaitIndex);
	ThreadLinkHolder link(fConn);
	link.StartMessage(radeonIoctlMsg);
	link.Attach<int>(fFd.Get());
	link.Attach<uint32_t>(DRM_IOCTL_SYNCOBJ_WAIT);
	link.Attach<uint64_t>(0);
	link.Attach<uint64_t>(timeout_nsec);
	link.Attach<uint32_t>(num_handles);
	link.Attach<uint32_t>(flags);
	link.Attach(handles, sizeof(uint32_t)*num_handles);
	status_t reply;
	link.FlushWithReply(reply);
	link.Read(first_signaled);
	// a timeout is a result (clients poll), not an error to report
	if (reply == ETIME)
		return reply;
	CheckRet(reply);
	return B_OK;
}

int RadeonGfxAccelerant::DrmSyncobjReset(const uint32_t *handles, uint32_t handle_count)
{
	CallTimer callTimer(kDrmSyncobjResetIndex);
	ThreadLinkHolder link(fConn);
	link.StartMessage(radeonIoctlMsg);
	link.Attach<int>(fFd.Get());
	link.Attach<uint32_t>(DRM_IOCTL_SYNCOBJ_RESET);
	link.Attach<uint32_t>(handle_count);
	link.Attach(handles, sizeof(uint32_t)*handle_count);
	status_t reply;
	link.FlushWithReply(reply);
	CheckRet(reply);
	return B_OK;
}

int RadeonGfxAccelerant::DrmSyncobjSignal(const uint32_t *handles, uint32_t handle_count)
{
	CallTimer callTimer(kDrmSyncobjSignalIndex);
	(void)handles;
	(void)handle_count;
	CheckRet(ENOSYS);
	return B_OK;
}

int RadeonGfxAccelerant::DrmSyncobjTimelineSignal(const uint32_t *handles, uint64_t *points, uint32_t handle_count)
{
	CallTimer callTimer(kDrmSyncobjTimelineSignalIndex);
	(void)handles;
	(void)points;
	(void)handle_count;
	CheckRet(ENOSYS);
	return B_OK;
}

/*!	Whether a timeline wait is already satisfied by the points the server
	publishes (RADEON_GFX_SYNCOBJ_POINTS_AREA). Only points above 0 count:
	those don't go back to unsignaled. Anything else asks the server.
*/
bool RadeonGfxAccelerant::TimelinePointsSignaled(const uint32_t *handles,
	const uint64_t *points, unsigned count, bool all, uint32_t *firstSignaled)
{
	const volatile uint64 *published = SyncobjPoints();
	if (published == NULL || count == 0)
		return false;
	for (unsigned i = 0; i < count; i++) {
		bool signaled = points[i] > 0
			&& handles[i] < RADEON_GFX_SYNCOBJ_POINTS_COUNT
			&& published[handles[i]] >= points[i];
		if (signaled && !all) {
			if (firstSignaled != NULL)
				*firstSignaled = i;
			return true;
		}
		if (!signaled && all)
			return false;
	}
	if (!all)
		return false;
	if (firstSignaled != NULL)
		*firstSignaled = 0;
	return true;
}

// the server's area of this team, cloned once per process
const volatile uint64 *RadeonGfxAccelerant::SyncobjPoints()
{
	static const volatile uint64 *sPoints = NULL;
	static int32 sAttempts = 0;
	if (sPoints != NULL)
		return sPoints;
	BAutolock lock(sContextLock);
	// the server creates it with the team's state, at the first connection
	if (sPoints != NULL || sAttempts >= 16)
		return sPoints;
	sAttempts++;
	// every wait goes to the server (debugging)
	if (getenv("RADEONGFX_NO_SYNCOBJ_POINTS") != NULL) {
		sAttempts = 16;
		return NULL;
	}
	char name[B_OS_NAME_LENGTH];
	snprintf(name, sizeof(name), "%s%" B_PRId32, RADEON_GFX_SYNCOBJ_POINTS_AREA,
		getpid());
	area_id area = find_area(name);
	if (area < B_OK)
		return NULL;
	void *address;
	// not the server's name: find_area() must not find the clone
	area_id clone = clone_area("radeon_gfx syncobj points clone", &address,
		B_ANY_ADDRESS, B_READ_AREA, area);
	if (clone < B_OK)
		return NULL;
	sPoints = (const volatile uint64*)address;
	return sPoints;
}

int RadeonGfxAccelerant::DrmSyncobjTimelineWait(uint32_t *handles, uint64_t *points, unsigned num_handles, int64_t timeout_nsec, unsigned flags, uint32_t *first_signaled)
{
	CallTimer callTimer(kDrmSyncobjTimelineWaitIndex);
	if (TimelinePointsSignaled(handles, points, num_handles,
			(flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL) != 0, first_signaled)) {
		return B_OK;
	}
	ThreadLinkHolder link(fConn);
	link.StartMessage(radeonIoctlMsg);
	link.Attach<int>(fFd.Get());
	link.Attach<uint32_t>(DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT);
	link.Attach<uint64_t>(0); // handles
	link.Attach<uint64_t>(0); // points
	link.Attach<uint64_t>(timeout_nsec);
	link.Attach<uint32_t>(num_handles);
	link.Attach<uint32_t>(flags);
	link.Attach(handles, sizeof(uint32_t)*num_handles);
	link.Attach(points, sizeof(uint64_t)*num_handles);
	status_t reply;
	link.FlushWithReply(reply);
	link.Read(first_signaled);
	// a timeout is a result (clients poll), not an error to report
	if (reply == ETIME)
		return reply;
	CheckRet(reply);
	return B_OK;
}

int RadeonGfxAccelerant::DrmSyncobjQuery(uint32_t *handles, uint64_t *points, uint32_t handle_count, uint32_t flags)
{
	CallTimer callTimer(kDrmSyncobjQueryIndex);
	(void)handles;
	(void)points;
	(void)handle_count;
	(void)flags;
	CheckRet(ENOSYS);
	return B_OK;
}

int RadeonGfxAccelerant::DrmSyncobjTransfer(uint32_t dst_handle, uint64_t dst_point, uint32_t src_handle, uint64_t src_point, uint32_t flags)
{
	CallTimer callTimer(kDrmSyncobjTransferIndex);
	ThreadLinkHolder link(fConn);
	link.StartMessage(radeonIoctlMsg);
	link.Attach<int>(fFd.Get());
	link.Attach<uint32_t>(DRM_IOCTL_SYNCOBJ_TRANSFER);
	link.Attach<uint32_t>(src_handle);
	link.Attach<uint32_t>(dst_handle);
	link.Attach<uint64_t>(src_point);
	link.Attach<uint64_t>(dst_point);
	link.Attach<uint32_t>(flags);
	link.Attach<uint32_t>(0); // pad
	// One-way: the server handles the messages of this thread in order, so
	// everything this thread sends afterwards sees the transfer. Other
	// threads wait for the point with WAIT_FOR_SUBMIT. Errors are logged by
	// the server.
	link.Flush();
	return B_OK;
}

int RadeonGfxAccelerant::DrmSyncobjAccumulate(uint32_t syncobj1, uint32_t syncobj2, uint64_t point)
{
	CallTimer callTimer(kDrmSyncobjAccumulateIndex);
	ThreadLinkHolder link(fConn);
	link.StartMessage(radeonIoctlMsg);
	link.Attach<int>(fFd.Get());
	link.Attach<uint32_t>(DRM_IOCTL_SYNCOBJ_ACCUMULATE);
	link.Attach<uint32_t>(syncobj1);
	link.Attach<uint32_t>(syncobj2);
	link.Attach<uint64_t>(point);
	status_t reply;
	link.FlushWithReply(reply);
	CheckRet(reply);
	return B_OK;
}


// #pragma mark - AMDGPU

int RadeonGfxAccelerant::AmdgpuQueryInfo(struct drm_amdgpu_info *info)
{
	CallTimer callTimer(kAmdgpuQueryInfoIndex);
	ThreadLinkHolder link(fConn);
	link.StartMessage(radeonIoctlMsg);
	link.Attach<int>(fFd.Get());
	link.Attach<uint32_t>(DRM_COMMAND_BASE + DRM_AMDGPU_INFO);
	link.Attach(info->return_size);
	link.Attach(info->query);
	switch (info->query) {
		case AMDGPU_INFO_ACCEL_WORKING:
		case AMDGPU_INFO_VRAM_USAGE:
		case AMDGPU_INFO_GTT_USAGE:
		case AMDGPU_INFO_GDS_CONFIG:
		case AMDGPU_INFO_VRAM_GTT:
		case AMDGPU_INFO_DEV_INFO:
		case AMDGPU_INFO_VIS_VRAM_USAGE:
		case AMDGPU_INFO_MEMORY:
		case AMDGPU_INFO_TIMESTAMP:
		case AMDGPU_INFO_NUM_BYTES_MOVED:
		case AMDGPU_INFO_NUM_EVICTIONS:
		case AMDGPU_INFO_VCE_CLOCK_TABLE:
		case AMDGPU_INFO_NUM_VRAM_CPU_PAGE_FAULTS:
		case AMDGPU_INFO_VRAM_LOST_COUNTER:
		case AMDGPU_INFO_RAS_ENABLED_FEATURES:
			// no input arguments
			break;
		case AMDGPU_INFO_CRTC_FROM_ID:
			link.Attach(info->mode_crtc);
			break;
		case AMDGPU_INFO_HW_IP_INFO:
		case AMDGPU_INFO_HW_IP_COUNT:
			link.Attach(info->query_hw_ip);
			break;
		case AMDGPU_INFO_READ_MMR_REG:
			link.Attach(info->read_mmr_reg);
			break;
		case AMDGPU_INFO_FW_VERSION:
			link.Attach(info->query_fw);
			break;
		case AMDGPU_INFO_VBIOS:
			link.Attach(info->vbios_info);
			break;
		case AMDGPU_INFO_SENSOR:
			link.Attach(info->sensor_info);
			break;
		case AMDGPU_INFO_VIDEO_CAPS:
			link.Attach(info->video_cap);
			break;
		default:
			fprintf(stderr, "RadeonGfxAccelerant::AmdgpuQueryInfo: unknown info->query: %" PRIu32 "\n", info->query);
			CheckRet(ENOSYS);
	}
	status_t reply;
	link.FlushWithReply(reply);
	uint32 replySize;
	link.Read(&replySize);
	link.Read((void*)(addr_t)info->return_pointer, replySize);
	CheckRet(reply);
	return B_OK;
}

int RadeonGfxAccelerant::AmdgpuBoAlloc(struct amdgpu_bo_alloc_request *alloc_buffer, uint32_t *buf_handle)
{
	CallTimer callTimer(kAmdgpuBoAllocIndex);
	ThreadLinkHolder link(fConn);
	link.StartMessage(radeonIoctlMsg);
	link.Attach<int>(fFd.Get());
	link.Attach<uint32_t>(DRM_COMMAND_BASE + DRM_AMDGPU_GEM_CREATE);
	link.Attach<uint64_t>(alloc_buffer->alloc_size);
	link.Attach<uint64_t>(alloc_buffer->phys_alignment);
	link.Attach<uint64_t>(alloc_buffer->preferred_heap);
	link.Attach<uint64_t>(alloc_buffer->flags);
	status_t reply;
	link.FlushWithReply(reply);
	link.Read(buf_handle);
	uint32_t pad;
	link.Read(&pad);
	CheckRet(reply);
	return B_OK;
}

int RadeonGfxAccelerant::AmdgpuCreateBoFromUserMem(void *cpu, uint64_t size, uint32_t *buf_handle)
{
	CallTimer callTimer(kAmdgpuCreateBoFromUserMemIndex);
	ThreadLinkHolder link(fConn);

	area_id area = area_for(cpu);
	CheckRet(area);
	area_info info;
	CheckRet(get_area_info(area, &info));

	link.StartMessage(radeonIoctlMsg);
	link.Attach<int>(fFd.Get());
	link.Attach<uint32_t>(DRM_COMMAND_BASE + DRM_AMDGPU_GEM_USERPTR);

	link.Attach<uint64_t>((addr_t)cpu - (addr_t)info.address);
	link.Attach<uint64_t>(size);
	link.Attach<uint32_t>(0); // flags
	link.Attach<uint32_t>(area);
	status_t reply;
	link.FlushWithReply(reply);
	link.Read(buf_handle);
	CheckRet(reply);
	return B_OK;
}

int RadeonGfxAccelerant::AmdgpuBoQueryInfo(uint32_t bo, struct amdgpu_bo_info *info)
{
	CallTimer callTimer(kAmdgpuBoQueryInfoIndex);
	ThreadLinkHolder link(fConn);
	struct drm_amdgpu_gem_metadata metadata {};
	struct drm_amdgpu_gem_create_in bo_info {};
	status_t reply;

	link.StartMessage(radeonIoctlMsg);
	link.Attach<int>(fFd.Get());
	link.Attach<uint32_t>(DRM_COMMAND_BASE + DRM_AMDGPU_GEM_METADATA);
	link.Attach<uint32_t>(bo);
	link.Attach<uint32_t>(AMDGPU_GEM_METADATA_OP_GET_METADATA);
	link.FlushWithReply(reply);
	link.Read(&metadata.data);
	CheckRet(reply);

	link.StartMessage(radeonIoctlMsg);
	link.Attach<int>(fFd.Get());
	link.Attach<uint32_t>(DRM_COMMAND_BASE + DRM_AMDGPU_GEM_OP);
	link.Attach<uint32_t>(bo);
	link.Attach<uint32_t>(AMDGPU_GEM_OP_GET_GEM_CREATE_INFO);
	link.FlushWithReply(reply);
	link.Read(&bo_info);

	memset(info, 0, sizeof(*info));
	info->alloc_size = bo_info.bo_size;
	info->phys_alignment = bo_info.alignment;
	info->preferred_heap = bo_info.domains;
	info->alloc_flags = bo_info.domain_flags;
	info->metadata.flags = metadata.data.flags;
	info->metadata.tiling_info = metadata.data.tiling_info;

	info->metadata.size_metadata = metadata.data.data_size_bytes;
	memcpy(info->metadata.umd_metadata, metadata.data.data, metadata.data.data_size_bytes);

	CheckRet(reply);
	return B_OK;
}

int RadeonGfxAccelerant::AmdgpuBoSetMetadata(uint32_t bo, struct amdgpu_bo_metadata *info)
{
	CallTimer callTimer(kAmdgpuBoSetMetadataIndex);
	ThreadLinkHolder link(fConn);
	struct drm_amdgpu_gem_metadata args {};

	args.handle = bo;
	args.op = AMDGPU_GEM_METADATA_OP_SET_METADATA;
	args.data.flags = info->flags;
	args.data.tiling_info = info->tiling_info;

	if (info->size_metadata > sizeof(args.data.data))
		return EINVAL;

	args.data.data_size_bytes = info->size_metadata;
	memcpy(args.data.data, info->umd_metadata, info->size_metadata);

	link.StartMessage(radeonIoctlMsg);
	link.Attach<int>(fFd.Get());
	link.Attach<uint32_t>(DRM_COMMAND_BASE + DRM_AMDGPU_GEM_METADATA);
	link.Attach(args);
	status_t reply;
	link.FlushWithReply(reply);
	CheckRet(reply);
	return B_OK;
}

int RadeonGfxAccelerant::AmdgpuBoVaOpRaw(uint32_t bo, uint64_t offset, uint64_t size, uint64_t addr, uint64_t flags, uint32_t ops)
{
	CallTimer callTimer(kAmdgpuBoVaOpRawIndex);
	ThreadLinkHolder link(fConn);
	link.StartMessage(radeonIoctlMsg);
	link.Attach<int>(fFd.Get());
	link.Attach<uint32_t>(DRM_COMMAND_BASE + DRM_AMDGPU_GEM_VA);
	link.Attach<uint32_t>(bo);
	link.Attach<uint32_t>(0); // pad
	link.Attach<uint32_t>(ops);
	link.Attach<uint32_t>(flags);
	link.Attach<uint64_t>(addr);
	link.Attach<uint64_t>(offset);
	link.Attach<uint64_t>(size);
	status_t reply;
	link.FlushWithReply(reply);
	CheckRet(reply);
	return B_OK;
}

int RadeonGfxAccelerant::AmdgpuBoCpuMap(uint32_t bo, void **cpu)
{
	CallTimer callTimer(kAmdgpuBoCpuMapIndex);
	void *adr = DrmMmap(NULL, 0, 0, 0, (off_t)bo);
	if (adr == NULL) return B_ERROR;
	*cpu = adr;
	return B_OK;
}

int RadeonGfxAccelerant::AmdgpuCsSubmitRaw(uint32_t context_id, uint32_t bo_list_handle, int num_chunks, struct drm_amdgpu_cs_chunk *chunks, uint64_t *seq_no)
{
	CallTimer callTimer(kAmdgpuCsSubmitRawIndex);
	ThreadLinkHolder link(fConn);
	link.StartMessage(radeonIoctlMsg);
	link.Attach<int>(fFd.Get());
	link.Attach<uint32_t>(DRM_COMMAND_BASE + DRM_AMDGPU_CS);
	link.Attach<uint32_t>(context_id);
	link.Attach<uint32_t>(bo_list_handle);
	link.Attach<uint32_t>(num_chunks);
	link.Attach<uint32_t>(0); // flags
	link.Attach<uint64_t>(0); // chunks
	for (int i = 0; i < num_chunks; i++) {
		link.Attach(chunks[i]);
	}
	for (int i = 0; i < num_chunks; i++) {
		auto &chunk = chunks[i];
		switch(chunk.chunk_id) {
			case AMDGPU_CHUNK_ID_IB:
			case AMDGPU_CHUNK_ID_FENCE:
			case AMDGPU_CHUNK_ID_DEPENDENCIES:
			case AMDGPU_CHUNK_ID_SYNCOBJ_IN:
			case AMDGPU_CHUNK_ID_SYNCOBJ_OUT:
			case AMDGPU_CHUNK_ID_SYNCOBJ_TIMELINE_WAIT:
			case AMDGPU_CHUNK_ID_SYNCOBJ_TIMELINE_SIGNAL: {
				link.Attach((void*)chunk.chunk_data, 4*chunk.length_dw);
				break;
			}
			case AMDGPU_CHUNK_ID_BO_HANDLES: {
				auto &chunk_data_handles = *(drm_amdgpu_bo_list_in*)chunk.chunk_data;
				link.Attach(chunk_data_handles);
				link.Attach((void*)chunk_data_handles.bo_info_ptr, chunk_data_handles.bo_info_size*chunk_data_handles.bo_number);
				break;
			}
		}
	}
	status_t reply;
	link.FlushWithReply(reply);
	link.Read(seq_no);
	CheckRet(reply);
	return B_OK;
}

int RadeonGfxAccelerant::AmdgpuWaitCs(uint32_t ctx_id, unsigned ip, unsigned ip_instance, uint32_t ring, uint64_t handle, uint64_t timeout_ns, bool *busy)
{
	CallTimer callTimer(kAmdgpuWaitCsIndex);
	ThreadLinkHolder link(fConn);
	link.StartMessage(radeonIoctlMsg);
	link.Attach<int>(fFd.Get());
	link.Attach<uint32_t>(DRM_COMMAND_BASE + DRM_AMDGPU_WAIT_CS);
	link.Attach<uint64_t>(handle);
	link.Attach<uint64_t>(timeout_ns);
	link.Attach<uint32_t>(ip);
	link.Attach<uint32_t>(ip_instance);
	link.Attach<uint32_t>(ring);
	link.Attach<uint32_t>(ctx_id);
	status_t reply;
	link.FlushWithReply(reply);
	uint64_t status;
	link.Read(&status);
	*busy = status != 0;
	CheckRet(reply);
	return B_OK;
}

int RadeonGfxAccelerant::AmdgpuCtxRaw(union drm_amdgpu_ctx *args)
{
	CallTimer callTimer(kAmdgpuCtxRawIndex);
	// the server keeps the contexts: a GPU reset marks them, and RADV asks
	// with AMDGPU_CTX_OP_QUERY_STATE2 after every wait whether its device is
	// lost; without a reset since the context's creation, that's known here
	// args is a union: the reply overwrites the request
	const uint32 op = args->in.op;
	const uint32 contextId = args->in.ctx_id;
	int32 resetCounter = fResetCounter != NULL ? *fResetCounter : -1;
	if (fResetCounter != NULL && (op == AMDGPU_CTX_OP_QUERY_STATE
			|| op == AMDGPU_CTX_OP_QUERY_STATE2)) {
		BAutolock lock(sContextLock);
		auto it = sContextResetCounters.find(contextId);
		if (it != sContextResetCounters.end() && it->second == resetCounter) {
			memset(&args->out, 0, sizeof(args->out));
			return B_OK;
		}
	}

	ThreadLinkHolder link(fConn);
	link.StartMessage(radeonIoctlMsg);
	link.Attach<int>(fFd.Get());
	link.Attach<uint32_t>(DRM_COMMAND_BASE + DRM_AMDGPU_CTX);
	link.Attach(&args->in, sizeof(args->in));
	status_t reply;
	link.FlushWithReply(reply);
	link.Read(&args->out, sizeof(args->out));
	CheckRet(reply);

	if (fResetCounter != NULL) {
		BAutolock lock(sContextLock);
		if (op == AMDGPU_CTX_OP_ALLOC_CTX) {
			// read before the allocation: a reset in between makes the next
			// query ask the server
			sContextResetCounters[args->out.alloc.ctx_id] = resetCounter;
		} else if (op == AMDGPU_CTX_OP_FREE_CTX)
			sContextResetCounters.erase(contextId);
	}
	return B_OK;
}


// #pragma mark - Display

status_t RadeonGfxAccelerant::DisplayGetConsumer(int32 crtc, BMessenger &consumer)
{
	CallTimer callTimer(kDisplayGetConsumerIndex);
	ThreadLinkHolder link(fConn);
	link.StartMessage(radeonGetDisplayConsumer);
	link.Attach(crtc);
	status_t reply;
	link.FlushWithReply(reply);
	link.Read(&consumer);
	CheckRet(reply);
	return B_OK;
}

status_t RadeonGfxAccelerant::DisplayUpdateCursor(int32 crtc, const CursorUpdateInfo &info)
{
	CallTimer callTimer(kDisplayUpdateCursorIndex);
	ThreadLinkHolder link(fConn);
	link.StartMessage(radeonUpdateCursor);
	link.Attach(crtc);
	link.Attach(info.valid.val);
	if (info.valid.enabled) {
		link.Attach(info.enabled);
	}
	if (info.valid.pos) {
		link.Attach(info.x);
		link.Attach(info.y);
	}
	if (info.valid.org) {
		link.Attach(info.orgX);
		link.Attach(info.orgY);
	}
	if (info.valid.format) {
		link.Attach(info.bytesPerRow);
		link.Attach(info.width);
		link.Attach(info.height);
		link.Attach<int32>(info.colorSpace);
	}
	if (info.valid.format) {
		size_t size = info.bytesPerRow*info.height;
		link.Attach(info.buffer, size);
	}
	status_t reply;
	link.FlushWithReply(reply);
	CheckRet(reply);
	return B_OK;
}


// #pragma mark -

status_t instantiate_accelerant(Accelerant **outAcc, int fd)
{
	ObjectDeleter<RadeonGfxAccelerant> acc(new(std::nothrow) RadeonGfxAccelerant(fd));
	if (!acc.IsSet()) return B_NO_MEMORY;
	CheckRet(acc->InitCheck());
	*outAcc = acc.Detach();
	return B_OK;
}
