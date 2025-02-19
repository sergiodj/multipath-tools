/*
 * snippets copied from device-mapper dmsetup.c
 * Copyright (c) 2004, 2005 Christophe Varoqui
 * Copyright (c) 2005 Kiyoshi Ueda, NEC
 * Copyright (c) 2005 Patrick Caulfield, Redhat
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <libdevmapper.h>
#include <ctype.h>
#include <unistd.h>
#include <errno.h>
#include <syslog.h>
#include <sys/sysmacros.h>
#include <linux/dm-ioctl.h>

#include "util.h"
#include "checkers.h"
#include "vector.h"
#include "structs.h"
#include "debug.h"
#include "devmapper.h"
#include "sysfs.h"
#include "config.h"
#include "wwids.h"
#include "version.h"
#include "time-util.h"

#include "log_pthread.h"
#include <sys/types.h>
#include <time.h>

#define MAX_WAIT 5
#define LOOPS_PER_SEC 5

#define INVALID_VERSION ~0U
static unsigned int dm_library_version[3] = { INVALID_VERSION, };
static unsigned int dm_kernel_version[3] = { INVALID_VERSION, };
static unsigned int dm_mpath_target_version[3] = { INVALID_VERSION, };

static pthread_once_t dm_initialized = PTHREAD_ONCE_INIT;
static pthread_once_t versions_initialized = PTHREAD_ONCE_INIT;
static pthread_mutex_t libmp_dm_lock = PTHREAD_MUTEX_INITIALIZER;

static int dm_conf_verbosity;

#ifdef LIBDM_API_DEFERRED
static int dm_cancel_remove_partmaps(const char * mapname);
#define __DR_UNUSED__ /* empty */
#else
#define __DR_UNUSED__ __attribute__((unused))
#endif

static int do_foreach_partmaps(const char * mapname,
			       int (*partmap_func)(const char *, void *),
			       void *data);

#ifndef LIBDM_API_COOKIE
static inline int dm_task_set_cookie(struct dm_task *dmt, uint32_t *c, int a)
{
	return 1;
}

static void libmp_udev_wait(unsigned int c)
{
}

static void dm_udev_set_sync_support(int c)
{
}
#else
static void libmp_udev_wait(unsigned int c)
{
	pthread_mutex_lock(&libmp_dm_lock);
	pthread_cleanup_push(cleanup_mutex, &libmp_dm_lock);
	dm_udev_wait(c);
	pthread_cleanup_pop(1);
}
#endif

int libmp_dm_task_run(struct dm_task *dmt)
{
	int r;

	pthread_mutex_lock(&libmp_dm_lock);
	pthread_cleanup_push(cleanup_mutex, &libmp_dm_lock);
	r = dm_task_run(dmt);
	pthread_cleanup_pop(1);
	return r;
}

__attribute__((format(printf, 4, 5))) static void
dm_write_log (int level, const char *file, int line, const char *f, ...)
{
	va_list ap;

	/*
	 * libdm uses the same log levels as syslog,
	 * except that EMERG/ALERT are not used
	 */
	if (level > LOG_DEBUG)
		level = LOG_DEBUG;

	if (level > dm_conf_verbosity)
		return;

	va_start(ap, f);
	if (logsink != LOGSINK_SYSLOG) {
		if (logsink == LOGSINK_STDERR_WITH_TIME) {
			struct timespec ts;
			char buff[32];

			get_monotonic_time(&ts);
			safe_sprintf(buff, "%ld.%06ld",
				     (long)ts.tv_sec, ts.tv_nsec/1000);
			fprintf(stderr, "%s | ", buff);
		}
		fprintf(stderr, "libdevmapper: %s(%i): ", file, line);
		vfprintf(stderr, f, ap);
		fprintf(stderr, "\n");
	} else {
		condlog(level >= LOG_ERR ? level - LOG_ERR : 0,
			"libdevmapper: %s(%i): ", file, line);
		log_safe(level, f, ap);
	}
	va_end(ap);

	return;
}

static void dm_init(int v)
{
	/*
	 * This maps libdm's standard loglevel _LOG_WARN (= 4), which is rather
	 * quiet in practice, to multipathd's default verbosity 2
	 */
	dm_conf_verbosity = v + 2;
	dm_log_init(&dm_write_log);
}

static void init_dm_library_version(void)
{
	char version[64];
	unsigned int v[3];

	dm_get_library_version(version, sizeof(version));
	if (sscanf(version, "%u.%u.%u ", &v[0], &v[1], &v[2]) != 3) {
		condlog(0, "invalid libdevmapper version %s", version);
		return;
	}
	memcpy(dm_library_version, v, sizeof(dm_library_version));
	condlog(3, "libdevmapper version %u.%.2u.%.2u",
		dm_library_version[0], dm_library_version[1],
		dm_library_version[2]);
}

static int
dm_lib_prereq (void)
{

#if defined(LIBDM_API_HOLD_CONTROL)
	unsigned int minv[3] = {1, 2, 111};
#elif defined(LIBDM_API_GET_ERRNO)
	unsigned int minv[3] = {1, 2, 99};
#elif defined(LIBDM_API_DEFERRED)
	unsigned int minv[3] = {1, 2, 89};
#elif defined(DM_SUBSYSTEM_UDEV_FLAG0)
	unsigned int minv[3] = {1, 2, 82};
#elif defined(LIBDM_API_COOKIE)
	unsigned int minv[3] = {1, 2, 38};
#else
	unsigned int minv[3] = {1, 2, 8};
#endif

	if (VERSION_GE(dm_library_version, minv))
		return 0;
	condlog(0, "libdevmapper version must be >= %u.%.2u.%.2u",
		minv[0], minv[1], minv[2]);
	return 1;
}

static void init_dm_drv_version(void)
{
	char buff[64];
	unsigned int v[3];

	if (!dm_driver_version(buff, sizeof(buff))) {
		condlog(0, "cannot get kernel dm version");
		return;
	}
	if (sscanf(buff, "%u.%u.%u ", &v[0], &v[1], &v[2]) != 3) {
		condlog(0, "invalid kernel dm version '%s'", buff);
		return;
	}
	memcpy(dm_kernel_version, v, sizeof(dm_library_version));
	condlog(3, "kernel device mapper v%u.%u.%u",
		dm_kernel_version[0],
		dm_kernel_version[1],
		dm_kernel_version[2]);
}

static int dm_tgt_version (unsigned int *version, char *str)
{
	int r = 2;
	struct dm_task *dmt;
	struct dm_versions *target;
	struct dm_versions *last_target;
	unsigned int *v;

	/*
	 * We have to call dm_task_create() and not libmp_dm_task_create()
	 * here to avoid a recursive invocation of
	 * pthread_once(&dm_initialized), which would cause a deadlock.
	 */
	if (!(dmt = dm_task_create(DM_DEVICE_LIST_VERSIONS)))
		return 1;

	dm_task_no_open_count(dmt);

	if (!libmp_dm_task_run(dmt)) {
		dm_log_error(2, DM_DEVICE_LIST_VERSIONS, dmt);
		condlog(0, "Can not communicate with kernel DM");
		goto out;
	}
	target = dm_task_get_versions(dmt);

	do {
		last_target = target;
		if (!strncmp(str, target->name, strlen(str))) {
			r = 1;
			break;
		}
		target = (void *) target + target->next;
	} while (last_target != target);

	if (r == 2) {
		condlog(0, "DM %s kernel driver not loaded", str);
		goto out;
	}
	v = target->version;
	version[0] = v[0];
	version[1] = v[1];
	version[2] = v[2];
	r = 0;
out:
	dm_task_destroy(dmt);
	return r;
}

static void init_dm_mpath_version(void)
{
	if (!dm_tgt_version(dm_mpath_target_version, TGT_MPATH))
		condlog(3, "DM multipath kernel driver v%u.%u.%u",
			dm_mpath_target_version[0],
			dm_mpath_target_version[1],
			dm_mpath_target_version[2]);
}

static int dm_tgt_prereq (unsigned int *ver)
{
	unsigned int minv[3] = {1, 0, 3};

	if (VERSION_GE(dm_mpath_target_version, minv)) {
		if (ver) {
			ver[0] = dm_mpath_target_version[0];
			ver[1] = dm_mpath_target_version[1];
			ver[2] = dm_mpath_target_version[2];
		}
		return 0;
	}

	condlog(0, "DM multipath kernel driver must be >= v%u.%u.%u",
		minv[0], minv[1], minv[2]);
	return 1;
}

static void _init_versions(void)
{
	/* Can't use condlog here because of how VERSION_STRING is defined */
	if (3 <= libmp_verbosity)
		dlog(3, VERSION_STRING);
	init_dm_library_version();
	init_dm_drv_version();
	init_dm_mpath_version();
}

static int init_versions(void) {
	pthread_once(&versions_initialized, _init_versions);
	return (dm_library_version[0] == INVALID_VERSION ||
		dm_kernel_version[0] == INVALID_VERSION ||
		dm_mpath_target_version[0] == INVALID_VERSION);
}

int dm_prereq(unsigned int *v)
{
	if (init_versions())
		return 1;
	if (dm_lib_prereq())
		return 1;
	return dm_tgt_prereq(v);
}

int libmp_get_version(int which, unsigned int version[3])
{
	unsigned int *src_version;

	init_versions();
	switch (which) {
	case DM_LIBRARY_VERSION:
		src_version = dm_library_version;
		break;
	case DM_KERNEL_VERSION:
		src_version = dm_kernel_version;
		break;
	case DM_MPATH_TARGET_VERSION:
		src_version = dm_mpath_target_version;
		break;
	case MULTIPATH_VERSION:
		version[0] = (VERSION_CODE >> 16) & 0xff;
		version[1] = (VERSION_CODE >> 8) & 0xff;
		version[2] = VERSION_CODE & 0xff;
		return 0;
	default:
		condlog(0, "%s: invalid value for 'which'", __func__);
		return 1;
	}
	if (src_version[0] == INVALID_VERSION)
		return 1;
	memcpy(version, src_version, 3 * sizeof(*version));
	return 0;
}

static int libmp_dm_udev_sync = 0;

void libmp_udev_set_sync_support(int on)
{
	libmp_dm_udev_sync = !!on;
}

static bool libmp_dm_init_called;
void libmp_dm_exit(void)
{
	if (!libmp_dm_init_called)
		return;

	/* switch back to default libdm logging */
	dm_log_init(NULL);
#ifdef LIBDM_API_HOLD_CONTROL
	/* make sure control fd is closed in dm_lib_release() */
	dm_hold_control_dev(0);
#endif
}

static void libmp_dm_init(void)
{
	unsigned int version[3];

	if (dm_prereq(version))
		exit(1);
	dm_init(libmp_verbosity);
#ifdef LIBDM_API_HOLD_CONTROL
	dm_hold_control_dev(1);
#endif
	dm_udev_set_sync_support(libmp_dm_udev_sync);
	libmp_dm_init_called = true;
}

static void _do_skip_libmp_dm_init(void)
{
}

void skip_libmp_dm_init(void)
{
	pthread_once(&dm_initialized, _do_skip_libmp_dm_init);
}

struct dm_task*
libmp_dm_task_create(int task)
{
	pthread_once(&dm_initialized, libmp_dm_init);
	return dm_task_create(task);
}

#define do_deferred(x) ((x) == DEFERRED_REMOVE_ON || (x) == DEFERRED_REMOVE_IN_PROGRESS)

static int
dm_simplecmd (int task, const char *name, int no_flush, int need_sync,
	      uint16_t udev_flags, int deferred_remove __DR_UNUSED__) {
	int r = 0;
	int udev_wait_flag = ((need_sync || udev_flags) &&
			      (task == DM_DEVICE_RESUME ||
			       task == DM_DEVICE_REMOVE));
	uint32_t cookie = 0;
	struct dm_task *dmt;

	if (!(dmt = libmp_dm_task_create (task)))
		return 0;

	if (!dm_task_set_name (dmt, name))
		goto out;

	dm_task_no_open_count(dmt);
	dm_task_skip_lockfs(dmt);	/* for DM_DEVICE_RESUME */
#ifdef LIBDM_API_FLUSH
	if (no_flush)
		dm_task_no_flush(dmt);		/* for DM_DEVICE_SUSPEND/RESUME */
#endif
#ifdef LIBDM_API_DEFERRED
	if (do_deferred(deferred_remove))
		dm_task_deferred_remove(dmt);
#endif
	if (udev_wait_flag &&
	    !dm_task_set_cookie(dmt, &cookie,
				DM_UDEV_DISABLE_LIBRARY_FALLBACK | udev_flags))
		goto out;

	r = libmp_dm_task_run (dmt);
	if (!r)
		dm_log_error(2, task, dmt);

	if (udev_wait_flag)
			libmp_udev_wait(cookie);
out:
	dm_task_destroy (dmt);
	return r;
}

int dm_simplecmd_flush (int task, const char *name, uint16_t udev_flags)
{
	return dm_simplecmd(task, name, 0, 1, udev_flags, 0);
}

int dm_simplecmd_noflush (int task, const char *name, uint16_t udev_flags)
{
	return dm_simplecmd(task, name, 1, 1, udev_flags, 0);
}

static int
dm_device_remove (const char *name, int needsync, int deferred_remove) {
	return dm_simplecmd(DM_DEVICE_REMOVE, name, 0, needsync, 0,
			    deferred_remove);
}

static int
dm_addmap (int task, const char *target, struct multipath *mpp,
	   char * params, int ro, uint16_t udev_flags) {
	int r = 0;
	struct dm_task *dmt;
	char *prefixed_uuid = NULL;
	uint32_t cookie = 0;

	if (task == DM_DEVICE_CREATE && strlen(mpp->wwid) == 0) {
		condlog(1, "%s: refusing to create map with empty WWID",
			mpp->alias);
		return 0;
	}

	/* Need to add this here to allow 0 to be passed in udev_flags */
	udev_flags |= DM_UDEV_DISABLE_LIBRARY_FALLBACK;

	if (!(dmt = libmp_dm_task_create (task)))
		return 0;

	if (!dm_task_set_name (dmt, mpp->alias))
		goto addout;

	if (!dm_task_add_target (dmt, 0, mpp->size, target, params))
		goto addout;

	if (ro)
		dm_task_set_ro(dmt);

	if (task == DM_DEVICE_CREATE) {
		if (asprintf(&prefixed_uuid, UUID_PREFIX "%s", mpp->wwid) < 0) {
			condlog(0, "cannot create prefixed uuid : %s",
				strerror(errno));
			goto addout;
		}
		if (!dm_task_set_uuid(dmt, prefixed_uuid))
			goto freeout;
		dm_task_skip_lockfs(dmt);
#ifdef LIBDM_API_FLUSH
		dm_task_no_flush(dmt);
#endif
	}

	if (mpp->attribute_flags & (1 << ATTR_MODE) &&
	    !dm_task_set_mode(dmt, mpp->mode))
		goto freeout;
	if (mpp->attribute_flags & (1 << ATTR_UID) &&
	    !dm_task_set_uid(dmt, mpp->uid))
		goto freeout;
	if (mpp->attribute_flags & (1 << ATTR_GID) &&
	    !dm_task_set_gid(dmt, mpp->gid))
		goto freeout;
	condlog(2, "%s: %s [0 %llu %s %s]", mpp->alias,
		task == DM_DEVICE_RELOAD ? "reload" : "addmap", mpp->size,
		target, params);

	dm_task_no_open_count(dmt);

	if (task == DM_DEVICE_CREATE &&
	    !dm_task_set_cookie(dmt, &cookie, udev_flags))
		goto freeout;

	r = libmp_dm_task_run (dmt);
	if (!r)
		dm_log_error(2, task, dmt);

	if (task == DM_DEVICE_CREATE)
			libmp_udev_wait(cookie);
freeout:
	if (prefixed_uuid)
		free(prefixed_uuid);

addout:
	dm_task_destroy (dmt);

	if (r)
		mpp->need_reload = false;
	return r;
}

static uint16_t build_udev_flags(const struct multipath *mpp, int reload)
{
	/* DM_UDEV_DISABLE_LIBRARY_FALLBACK is added in dm_addmap */
	return	(mpp->skip_kpartx == SKIP_KPARTX_ON ?
		 MPATH_UDEV_NO_KPARTX_FLAG : 0) |
		((count_active_pending_paths(mpp) == 0 ||
		  mpp->ghost_delay_tick > 0) ?
		 MPATH_UDEV_NO_PATHS_FLAG : 0) |
		(reload && !mpp->force_udev_reload ?
		 MPATH_UDEV_RELOAD_FLAG : 0);
}

int dm_addmap_create (struct multipath *mpp, char * params)
{
	int ro;
	uint16_t udev_flags = build_udev_flags(mpp, 0);

	for (ro = mpp->force_readonly ? 1 : 0; ro <= 1; ro++) {
		int err;

		if (dm_addmap(DM_DEVICE_CREATE, TGT_MPATH, mpp, params, ro,
			      udev_flags)) {
			if (unmark_failed_wwid(mpp->wwid) ==
			    WWID_FAILED_CHANGED)
				mpp->needs_paths_uevent = 1;
			return 1;
		}
		/*
		 * DM_DEVICE_CREATE is actually DM_DEV_CREATE + DM_TABLE_LOAD.
		 * Failing the second part leaves an empty map. Clean it up.
		 */
		err = errno;
		if (dm_map_present(mpp->alias)) {
			condlog(3, "%s: failed to load map (a path might be in use)", mpp->alias);
			dm_flush_map_nosync(mpp->alias);
		}
		if (err != EROFS) {
			condlog(3, "%s: failed to load map, error %d",
				mpp->alias, err);
			break;
		}
	}
	if (mark_failed_wwid(mpp->wwid) == WWID_FAILED_CHANGED)
		mpp->needs_paths_uevent = 1;
	return 0;
}

#define ADDMAP_RW 0
#define ADDMAP_RO 1

int dm_addmap_reload(struct multipath *mpp, char *params, int flush)
{
	int r = 0;
	uint16_t udev_flags = build_udev_flags(mpp, 1);

	/*
	 * DM_DEVICE_RELOAD cannot wait on a cookie, as
	 * the cookie will only ever be released after an
	 * DM_DEVICE_RESUME. So call DM_DEVICE_RESUME
	 * after each successful call to DM_DEVICE_RELOAD.
	 */
	if (!mpp->force_readonly)
		r = dm_addmap(DM_DEVICE_RELOAD, TGT_MPATH, mpp, params,
			      ADDMAP_RW, 0);
	if (!r) {
		if (!mpp->force_readonly && errno != EROFS)
			return 0;
		r = dm_addmap(DM_DEVICE_RELOAD, TGT_MPATH, mpp,
			      params, ADDMAP_RO, 0);
	}
	if (r)
		r = dm_simplecmd(DM_DEVICE_RESUME, mpp->alias, !flush,
				 1, udev_flags, 0);
	if (r)
		return r;

	/* If the resume failed, dm will leave the device suspended, and
	 * drop the new table, so doing a second resume will try using
	 * the original table */
	if (dm_is_suspended(mpp->alias))
		dm_simplecmd(DM_DEVICE_RESUME, mpp->alias, !flush, 1,
			     udev_flags, 0);
	return 0;
}

bool
has_dm_info(const struct multipath *mpp)
{
	return (mpp && mpp->dmi.exists != 0);
}

int
dm_get_info(const char *name, struct dm_info *info)
{
	int r = -1;
	struct dm_task *dmt;

	if (!name || !info)
		return r;

	if (!(dmt = libmp_dm_task_create(DM_DEVICE_INFO)))
		return r;

	if (!dm_task_set_name(dmt, name))
		goto out;

	dm_task_no_open_count(dmt);

	if (!libmp_dm_task_run(dmt)) {
		dm_log_error(3, DM_DEVICE_INFO, dmt);
		goto out;
	}

	if (!dm_task_get_info(dmt, info))
		goto out;

	if (!info->exists)
		goto out;

	r = 0;
out:
	dm_task_destroy(dmt);
	return r;
}

int dm_map_present(const char * str)
{
	struct dm_info info;

	return (dm_get_info(str, &info) == 0);
}

int dm_get_map(const char *name, unsigned long long *size, char **outparams)
{
	int r = DMP_ERR;
	struct dm_task *dmt;
	uint64_t start, length;
	char *target_type = NULL;
	char *params = NULL;

	if (!(dmt = libmp_dm_task_create(DM_DEVICE_TABLE)))
		return r;

	if (!dm_task_set_name(dmt, name))
		goto out;

	dm_task_no_open_count(dmt);

	errno = 0;
	if (!libmp_dm_task_run(dmt)) {
		dm_log_error(3, DM_DEVICE_TABLE, dmt);
		if (dm_task_get_errno(dmt) == ENXIO)
			r = DMP_NOT_FOUND;
		goto out;
	}

	r = DMP_NOT_FOUND;
	/* Fetch 1st target */
	if (dm_get_next_target(dmt, NULL, &start, &length,
			       &target_type, &params) != NULL || !params)
		/* more than one target or not found target */
		goto out;

	if (size)
		*size = length;

	if (!outparams)
		r = DMP_OK;
	else {
		*outparams = strdup(params);
		r = *outparams ? DMP_OK : DMP_ERR;
	}

out:
	dm_task_destroy(dmt);
	return r;
}

static int
dm_get_prefixed_uuid(const char *name, char *uuid, int uuid_len)
{
	struct dm_task *dmt;
	const char *uuidtmp;
	struct dm_info info;
	int r = 1;

	dmt = libmp_dm_task_create(DM_DEVICE_INFO);
	if (!dmt)
		return 1;

	if (uuid_len > 0)
		uuid[0] = '\0';

	if (!dm_task_set_name (dmt, name))
		goto uuidout;

	if (!libmp_dm_task_run(dmt)) {
		dm_log_error(3, DM_DEVICE_INFO, dmt);
		goto uuidout;
	}

	if (!dm_task_get_info(dmt, &info) ||
	    !info.exists)
		goto uuidout;

	uuidtmp = dm_task_get_uuid(dmt);
	if (uuidtmp)
		strlcpy(uuid, uuidtmp, uuid_len);

	r = 0;
uuidout:
	dm_task_destroy(dmt);
	return r;
}

int dm_get_uuid(const char *name, char *uuid, int uuid_len)
{
	char tmp[DM_UUID_LEN];

	if (dm_get_prefixed_uuid(name, tmp, sizeof(tmp)))
		return 1;

	if (!strncmp(tmp, UUID_PREFIX, UUID_PREFIX_LEN))
		strlcpy(uuid, tmp + UUID_PREFIX_LEN, uuid_len);
	else
		uuid[0] = '\0';

	return 0;
}

static int
is_mpath_part(const char *part_name, const char *map_name)
{
	char *p;
	char part_uuid[DM_UUID_LEN], map_uuid[DM_UUID_LEN];

	if (dm_get_prefixed_uuid(part_name, part_uuid, sizeof(part_uuid)))
		return 0;

	if (dm_get_prefixed_uuid(map_name, map_uuid, sizeof(map_uuid)))
		return 0;

	if (strncmp(part_uuid, "part", 4) != 0)
		return 0;

	p = strstr(part_uuid, UUID_PREFIX);
	if (p && !strcmp(p, map_uuid))
		return 1;

	return 0;
}

int dm_get_status(const char *name, char **outstatus)
{
	int r = DMP_ERR;
	struct dm_task *dmt;
	uint64_t start, length;
	char *target_type = NULL;
	char *status = NULL;

	if (!(dmt = libmp_dm_task_create(DM_DEVICE_STATUS)))
		return r;

	if (!dm_task_set_name(dmt, name))
		goto out;

	dm_task_no_open_count(dmt);

	errno = 0;
	if (!libmp_dm_task_run(dmt)) {
		dm_log_error(3, DM_DEVICE_STATUS, dmt);
		if (dm_task_get_errno(dmt) == ENXIO)
			r = DMP_NOT_FOUND;
		goto out;
	}

	r = DMP_NOT_FOUND;
	/* Fetch 1st target */
	if (dm_get_next_target(dmt, NULL, &start, &length,
			       &target_type, &status) != NULL)
		goto out;

	if (!target_type || strcmp(target_type, TGT_MPATH) != 0)
		goto out;

	if (!status) {
		condlog(2, "get null status.");
		goto out;
	}

	if (!outstatus)
		r = DMP_OK;
	else {
		*outstatus = strdup(status);
		r = *outstatus ? DMP_OK : DMP_ERR;
	}
out:
	if (r != DMP_OK)
		condlog(0, "%s: error getting map status string", name);

	dm_task_destroy(dmt);
	return r;
}

/*
 * returns:
 *    1 : match
 *    0 : no match
 *   -1 : empty map, or more than 1 target
 */
int dm_type(const char *name, char *type)
{
	int r = 0;
	struct dm_task *dmt;
	uint64_t start, length;
	char *target_type = NULL;
	char *params;

	if (!(dmt = libmp_dm_task_create(DM_DEVICE_TABLE)))
		return 0;

	if (!dm_task_set_name(dmt, name))
		goto out;

	dm_task_no_open_count(dmt);

	if (!libmp_dm_task_run(dmt)) {
		dm_log_error(3, DM_DEVICE_TABLE, dmt);
		goto out;
	}

	/* Fetch 1st target */
	if (dm_get_next_target(dmt, NULL, &start, &length,
			       &target_type, &params) != NULL)
		/* multiple targets */
		r = -1;
	else if (!target_type)
		r = -1;
	else if (!strcmp(target_type, type))
		r = 1;

out:
	dm_task_destroy(dmt);
	return r;
}

/*
 * returns:
 * 1  : is multipath device
 * 0  : is not multipath device
 * -1 : error
 */
int dm_is_mpath(const char *name)
{
	int r = -1;
	struct dm_task *dmt;
	struct dm_info info;
	uint64_t start, length;
	char *target_type = NULL;
	char *params;
	const char *uuid;

	if (!(dmt = libmp_dm_task_create(DM_DEVICE_TABLE)))
		goto out;

	if (!dm_task_set_name(dmt, name))
		goto out_task;

	dm_task_no_open_count(dmt);

	if (!libmp_dm_task_run(dmt)) {
		dm_log_error(3, DM_DEVICE_TABLE, dmt);
		goto out_task;
	}

	if (!dm_task_get_info(dmt, &info))
		goto out_task;

	r = 0;

	if (!info.exists)
		goto out_task;

	uuid = dm_task_get_uuid(dmt);

	if (!uuid || strncmp(uuid, UUID_PREFIX, UUID_PREFIX_LEN) != 0)
		goto out_task;

	/* Fetch 1st target */
	if (dm_get_next_target(dmt, NULL, &start, &length, &target_type,
			       &params) != NULL)
		/* multiple targets */
		goto out_task;

	if (!target_type || strcmp(target_type, TGT_MPATH) != 0)
		goto out_task;

	r = 1;
out_task:
	dm_task_destroy(dmt);
out:
	if (r < 0)
		condlog(3, "%s: dm command failed in %s: %s", name, __FUNCTION__, strerror(errno));
	return r;
}

/*
 * Return
 *   1 : map with uuid exists
 *   0 : map with uuid doesn't exist
 *  -1 : error
 */
int
dm_map_present_by_uuid(const char *uuid)
{
	struct dm_task *dmt;
	struct dm_info info;
	char prefixed_uuid[WWID_SIZE + UUID_PREFIX_LEN];
	int r = -1;

	if (!uuid || uuid[0] == '\0')
		return 0;

	if (safe_sprintf(prefixed_uuid, UUID_PREFIX "%s", uuid))
		goto out;

	if (!(dmt = libmp_dm_task_create(DM_DEVICE_INFO)))
		goto out;

	dm_task_no_open_count(dmt);

	if (!dm_task_set_uuid(dmt, prefixed_uuid))
		goto out_task;

	if (!libmp_dm_task_run(dmt)) {
		dm_log_error(3, DM_DEVICE_INFO, dmt);
		goto out_task;
	}

	if (!dm_task_get_info(dmt, &info))
		goto out_task;

	r = !!info.exists;

out_task:
	dm_task_destroy(dmt);
out:
	if (r < 0)
		condlog(3, "%s: dm command failed in %s: %s", uuid,
			__FUNCTION__, strerror(errno));
	return r;
}

static int
dm_dev_t (const char * mapname, char * dev_t, int len)
{
	struct dm_info info;

	if (dm_get_info(mapname, &info) != 0)
		return 1;

	if (snprintf(dev_t, len, "%i:%i", info.major, info.minor) > len)
		return 1;

	return 0;
}

int
dm_get_opencount (const char * mapname)
{
	int r = -1;
	struct dm_task *dmt;
	struct dm_info info;

	if (!(dmt = libmp_dm_task_create(DM_DEVICE_INFO)))
		return 0;

	if (!dm_task_set_name(dmt, mapname))
		goto out;

	if (!libmp_dm_task_run(dmt)) {
		dm_log_error(3, DM_DEVICE_INFO, dmt);
		goto out;
	}

	if (!dm_task_get_info(dmt, &info))
		goto out;

	if (!info.exists)
		goto out;

	r = info.open_count;
out:
	dm_task_destroy(dmt);
	return r;
}

int
dm_get_major_minor(const char *name, int *major, int *minor)
{
	struct dm_info info;

	if (dm_get_info(name, &info) != 0)
		return -1;

	*major = info.major;
	*minor = info.minor;
	return 0;
}

static int
has_partmap(const char *name __attribute__((unused)),
	    void *data __attribute__((unused)))
{
	return 1;
}

static int
partmap_in_use(const char *name, void *data)
{
	int part_count, *ret_count = (int *)data;
	int open_count = dm_get_opencount(name);

	if (ret_count)
		(*ret_count)++;
	part_count = 0;
	if (open_count) {
		if (do_foreach_partmaps(name, partmap_in_use, &part_count))
			return 1;
		if (open_count != part_count) {
			condlog(2, "%s: map in use", name);
			return 1;
		}
	}
	return 0;
}

int _dm_flush_map (const char * mapname, int need_sync, int deferred_remove,
		   int need_suspend, int retries)
{
	int r;
	int queue_if_no_path = 0;
	int udev_flags = 0;
	unsigned long long mapsize;
	char *params = NULL;

	if (dm_is_mpath(mapname) != 1)
		return 0; /* nothing to do */

	/* if the device currently has no partitions, do not
	   run kpartx on it if you fail to delete it */
	if (do_foreach_partmaps(mapname, has_partmap, NULL) == 0)
		udev_flags |= MPATH_UDEV_NO_KPARTX_FLAG;

	/* If you aren't doing a deferred remove, make sure that no
	 * devices are in use */
	if (!do_deferred(deferred_remove) && partmap_in_use(mapname, NULL))
			return 1;

	if (need_suspend &&
	    dm_get_map(mapname, &mapsize, &params) == DMP_OK &&
	    strstr(params, "queue_if_no_path")) {
		if (!dm_queue_if_no_path(mapname, 0))
			queue_if_no_path = 1;
		else
			/* Leave queue_if_no_path alone if unset failed */
			queue_if_no_path = -1;
	}
	free(params);
	params = NULL;

	if (dm_remove_partmaps(mapname, need_sync, deferred_remove))
		return 1;

	if (!do_deferred(deferred_remove) && dm_get_opencount(mapname)) {
		condlog(2, "%s: map in use", mapname);
		return 1;
	}

	do {
		if (need_suspend && queue_if_no_path != -1)
			dm_simplecmd_flush(DM_DEVICE_SUSPEND, mapname, 0);

		r = dm_device_remove(mapname, need_sync, deferred_remove);

		if (r) {
			if (do_deferred(deferred_remove)
			    && dm_map_present(mapname)) {
				condlog(4, "multipath map %s remove deferred",
					mapname);
				return 2;
			}
			condlog(4, "multipath map %s removed", mapname);
			return 0;
		} else if (dm_is_mpath(mapname) != 1) {
			condlog(4, "multipath map %s removed externally",
				mapname);
			return 0; /*we raced with someone else removing it */
		} else {
			condlog(2, "failed to remove multipath map %s",
				mapname);
			if (need_suspend && queue_if_no_path != -1) {
				dm_simplecmd_noflush(DM_DEVICE_RESUME,
						     mapname, udev_flags);
			}
		}
		if (retries)
			sleep(1);
	} while (retries-- > 0);

	if (queue_if_no_path == 1)
		dm_queue_if_no_path(mapname, 1);

	return 1;
}

#ifdef LIBDM_API_DEFERRED

int
dm_flush_map_nopaths(const char * mapname, int deferred_remove)
{
	return _dm_flush_map(mapname, 1, deferred_remove, 0, 0);
}

#else

int
dm_flush_map_nopaths(const char * mapname,
		     int deferred_remove __attribute__((unused)))
{
	return _dm_flush_map(mapname, 1, 0, 0, 0);
}

#endif

int dm_flush_maps (int need_suspend, int retries)
{
	int r = 1;
	struct dm_task *dmt;
	struct dm_names *names;
	unsigned next = 0;

	if (!(dmt = libmp_dm_task_create (DM_DEVICE_LIST)))
		return r;

	dm_task_no_open_count(dmt);

	if (!libmp_dm_task_run (dmt)) {
		dm_log_error(3, DM_DEVICE_LIST, dmt);
		goto out;
	}

	if (!(names = dm_task_get_names (dmt)))
		goto out;

	r = 0;
	if (!names->dev)
		goto out;

	do {
		if (need_suspend)
			r |= dm_suspend_and_flush_map(names->name, retries);
		else
			r |= dm_flush_map(names->name);
		next = names->next;
		names = (void *) names + next;
	} while (next);

out:
	dm_task_destroy (dmt);
	return r;
}

int
dm_message(const char * mapname, char * message)
{
	int r = 1;
	struct dm_task *dmt;

	if (!(dmt = libmp_dm_task_create(DM_DEVICE_TARGET_MSG)))
		return 1;

	if (!dm_task_set_name(dmt, mapname))
		goto out;

	if (!dm_task_set_sector(dmt, 0))
		goto out;

	if (!dm_task_set_message(dmt, message))
		goto out;

	dm_task_no_open_count(dmt);

	if (!libmp_dm_task_run(dmt)) {
		dm_log_error(2, DM_DEVICE_TARGET_MSG, dmt);
		goto out;
	}

	r = 0;
out:
	if (r)
		condlog(0, "DM message failed [%s]", message);

	dm_task_destroy(dmt);
	return r;
}

int
dm_fail_path(const char * mapname, char * path)
{
	char message[32];

	if (snprintf(message, 32, "fail_path %s", path) > 32)
		return 1;

	return dm_message(mapname, message);
}

int
dm_reinstate_path(const char * mapname, char * path)
{
	char message[32];

	if (snprintf(message, 32, "reinstate_path %s", path) > 32)
		return 1;

	return dm_message(mapname, message);
}

int
dm_queue_if_no_path(const char *mapname, int enable)
{
	char *message;

	if (enable)
		message = "queue_if_no_path";
	else
		message = "fail_if_no_path";

	return dm_message(mapname, message);
}

static int
dm_groupmsg (const char * msg, const char * mapname, int index)
{
	char message[32];

	if (snprintf(message, 32, "%s_group %i", msg, index) > 32)
		return 1;

	return dm_message(mapname, message);
}

int
dm_switchgroup(const char * mapname, int index)
{
	return dm_groupmsg("switch", mapname, index);
}

int
dm_enablegroup(const char * mapname, int index)
{
	return dm_groupmsg("enable", mapname, index);
}

int
dm_disablegroup(const char * mapname, int index)
{
	return dm_groupmsg("disable", mapname, index);
}

struct multipath *dm_get_multipath(const char *name)
{
	struct multipath *mpp = NULL;

	mpp = alloc_multipath();
	if (!mpp)
		return NULL;

	mpp->alias = strdup(name);

	if (!mpp->alias)
		goto out;

	if (dm_get_map(name, &mpp->size, NULL) != DMP_OK)
		goto out;

	if (dm_get_uuid(name, mpp->wwid, WWID_SIZE) != 0)
		condlog(2, "%s: failed to get uuid for %s", __func__, name);
	if (dm_get_info(name, &mpp->dmi) != 0)
		condlog(2, "%s: failed to get info for %s", __func__, name);

	return mpp;
out:
	free_multipath(mpp, KEEP_PATHS);
	return NULL;
}

int
dm_get_maps (vector mp)
{
	struct multipath * mpp;
	int r = 1;
	struct dm_task *dmt;
	struct dm_names *names;
	unsigned next = 0;

	if (!mp)
		return 1;

	if (!(dmt = libmp_dm_task_create(DM_DEVICE_LIST)))
		return 1;

	dm_task_no_open_count(dmt);

	if (!libmp_dm_task_run(dmt)) {
		dm_log_error(3, DM_DEVICE_LIST, dmt);
		goto out;
	}

	if (!(names = dm_task_get_names(dmt)))
		goto out;

	if (!names->dev) {
		r = 0; /* this is perfectly valid */
		goto out;
	}

	do {
		if (dm_is_mpath(names->name) != 1)
			goto next;

		mpp = dm_get_multipath(names->name);
		if (!mpp)
			goto out;

		if (!vector_alloc_slot(mp)) {
			free_multipath(mpp, KEEP_PATHS);
			goto out;
		}

		vector_set_slot(mp, mpp);
next:
		next = names->next;
		names = (void *) names + next;
	} while (next);

	r = 0;
	goto out;
out:
	dm_task_destroy (dmt);
	return r;
}

int
dm_geteventnr (const char *name)
{
	struct dm_info info;

	if (dm_get_info(name, &info) != 0)
		return -1;

	return info.event_nr;
}

int
dm_is_suspended(const char *name)
{
	struct dm_info info;

	if (dm_get_info(name, &info) != 0)
		return -1;

	return info.suspended;
}

char *
dm_mapname(int major, int minor)
{
	char * response = NULL;
	const char *map;
	struct dm_task *dmt;
	int r;

	if (!(dmt = libmp_dm_task_create(DM_DEVICE_STATUS)))
		return NULL;

	if (!dm_task_set_major(dmt, major) ||
	    !dm_task_set_minor(dmt, minor))
		goto bad;

	dm_task_no_open_count(dmt);
	r = libmp_dm_task_run(dmt);
	if (!r) {
		dm_log_error(2, DM_DEVICE_STATUS, dmt);
		goto bad;
	}

	map = dm_task_get_name(dmt);
	if (map && strlen(map))
		response = strdup((const char *)map);

	dm_task_destroy(dmt);
	return response;
bad:
	dm_task_destroy(dmt);
	condlog(0, "%i:%i: error fetching map name", major, minor);
	return NULL;
}

static int
do_foreach_partmaps (const char * mapname,
		     int (*partmap_func)(const char *, void *),
		     void *data)
{
	struct dm_task *dmt;
	struct dm_names *names;
	unsigned next = 0;
	char *params = NULL;
	unsigned long long size;
	char dev_t[32];
	int r = 1;
	char *p;

	if (!(dmt = libmp_dm_task_create(DM_DEVICE_LIST)))
		return 1;

	dm_task_no_open_count(dmt);

	if (!libmp_dm_task_run(dmt)) {
		dm_log_error(3, DM_DEVICE_LIST, dmt);
		goto out;
	}

	if (!(names = dm_task_get_names(dmt)))
		goto out;

	if (!names->dev) {
		r = 0; /* this is perfectly valid */
		goto out;
	}

	if (dm_dev_t(mapname, &dev_t[0], 32))
		goto out;

	do {
		if (
		    /*
		     * if there is only a single "linear" target
		     */
		    (dm_type(names->name, TGT_PART) == 1) &&

		    /*
		     * and the uuid of the target is a partition of the
		     * uuid of the multipath device
		     */
		    is_mpath_part(names->name, mapname) &&

		    /*
		     * and we can fetch the map table from the kernel
		     */
		    dm_get_map(names->name, &size, &params) == DMP_OK &&

		    /*
		     * and the table maps over the multipath map
		     */
		    (p = strstr(params, dev_t)) &&
		    !isdigit(*(p + strlen(dev_t)))
		   ) {
			if (partmap_func(names->name, data) != 0)
				goto out;
		}

		free(params);
		params = NULL;
		next = names->next;
		names = (void *) names + next;
	} while (next);

	r = 0;
out:
	free(params);
	dm_task_destroy (dmt);
	return r;
}

struct remove_data {
	int need_sync;
	int deferred_remove;
};

static int
remove_partmap(const char *name, void *data)
{
	struct remove_data *rd = (struct remove_data *)data;

	if (dm_get_opencount(name)) {
		dm_remove_partmaps(name, rd->need_sync, rd->deferred_remove);
		if (!do_deferred(rd->deferred_remove) &&
		    dm_get_opencount(name)) {
			condlog(2, "%s: map in use", name);
			return 1;
		}
	}
	condlog(4, "partition map %s removed", name);
	dm_device_remove(name, rd->need_sync, rd->deferred_remove);
	return 0;
}

int
dm_remove_partmaps (const char * mapname, int need_sync, int deferred_remove)
{
	struct remove_data rd = { need_sync, deferred_remove };
	return do_foreach_partmaps(mapname, remove_partmap, &rd);
}

#ifdef LIBDM_API_DEFERRED

static int
cancel_remove_partmap (const char *name, void *unused __attribute__((unused)))
{
	if (dm_get_opencount(name))
		dm_cancel_remove_partmaps(name);
	if (dm_message(name, "@cancel_deferred_remove") != 0)
		condlog(0, "%s: can't cancel deferred remove: %s", name,
			strerror(errno));
	return 0;
}

static int
dm_get_deferred_remove (const char * mapname)
{
	struct dm_info info;

	if (dm_get_info(mapname, &info) != 0)
		return -1;

	return info.deferred_remove;
}

static int
dm_cancel_remove_partmaps(const char * mapname) {
	return do_foreach_partmaps(mapname, cancel_remove_partmap, NULL);
}

int
dm_cancel_deferred_remove (struct multipath *mpp)
{
	int r = 0;

	if (!dm_get_deferred_remove(mpp->alias))
		return 0;
	if (mpp->deferred_remove == DEFERRED_REMOVE_IN_PROGRESS)
		mpp->deferred_remove = DEFERRED_REMOVE_ON;

	dm_cancel_remove_partmaps(mpp->alias);
	r = dm_message(mpp->alias, "@cancel_deferred_remove");
	if (r)
		condlog(0, "%s: can't cancel deferred remove: %s", mpp->alias,
				strerror(errno));
	else
		condlog(2, "%s: canceled deferred remove", mpp->alias);
	return r;
}

#else

int
dm_cancel_deferred_remove (struct multipath *mpp __attribute__((unused)))
{
	return 0;
}

#endif

struct rename_data {
	const char *old;
	char *new;
	char *delim;
};

static int
rename_partmap (const char *name, void *data)
{
	char *buff = NULL;
	int offset;
	struct rename_data *rd = (struct rename_data *)data;

	if (strncmp(name, rd->old, strlen(rd->old)) != 0)
		return 0;
	for (offset = strlen(rd->old); name[offset] && !(isdigit(name[offset])); offset++); /* do nothing */
	if (asprintf(&buff, "%s%s%s", rd->new, rd->delim, name + offset) >= 0) {
		dm_rename(name, buff, rd->delim, SKIP_KPARTX_OFF);
		free(buff);
		condlog(4, "partition map %s renamed", name);
	} else
		condlog(1, "failed to rename partition map %s", name);
	return 0;
}

int
dm_rename_partmaps (const char * old, char * new, char *delim)
{
	struct rename_data rd;

	rd.old = old;
	rd.new = new;

	if (delim)
		rd.delim = delim;
	else {
		if (isdigit(new[strlen(new)-1]))
			rd.delim = "p";
		else
			rd.delim = "";
	}
	return do_foreach_partmaps(old, rename_partmap, &rd);
}

int
dm_rename (const char * old, char * new, char *delim, int skip_kpartx)
{
	int r = 0;
	struct dm_task *dmt;
	uint32_t cookie = 0;
	uint16_t udev_flags = DM_UDEV_DISABLE_LIBRARY_FALLBACK | ((skip_kpartx == SKIP_KPARTX_ON)? MPATH_UDEV_NO_KPARTX_FLAG : 0);

	if (dm_rename_partmaps(old, new, delim))
		return r;

	if (!(dmt = libmp_dm_task_create(DM_DEVICE_RENAME)))
		return r;

	if (!dm_task_set_name(dmt, old))
		goto out;

	if (!dm_task_set_newname(dmt, new))
		goto out;

	dm_task_no_open_count(dmt);

	if (!dm_task_set_cookie(dmt, &cookie, udev_flags))
		goto out;
	r = libmp_dm_task_run(dmt);
	if (!r)
		dm_log_error(2, DM_DEVICE_RENAME, dmt);

	libmp_udev_wait(cookie);

out:
	dm_task_destroy(dmt);

	return r;
}

void dm_reassign_deps(char *table, const char *dep, const char *newdep)
{
	char *n, *newtable;
	const char *p;

	newtable = strdup(table);
	if (!newtable)
		return;
	p = strstr(newtable, dep);
	n = table + (p - newtable);
	strcpy(n, newdep);
	n += strlen(newdep);
	p += strlen(dep);
	strcat(n, p);
	free(newtable);
}

int dm_reassign_table(const char *name, char *old, char *new)
{
	int r = 0, modified = 0;
	uint64_t start, length;
	struct dm_task *dmt, *reload_dmt;
	char *target, *params = NULL;
	char *buff;
	void *next = NULL;

	if (!(dmt = libmp_dm_task_create(DM_DEVICE_TABLE)))
		return 0;

	if (!dm_task_set_name(dmt, name))
		goto out;

	dm_task_no_open_count(dmt);

	if (!libmp_dm_task_run(dmt)) {
		dm_log_error(3, DM_DEVICE_TABLE, dmt);
		goto out;
	}
	if (!(reload_dmt = libmp_dm_task_create(DM_DEVICE_RELOAD)))
		goto out;
	if (!dm_task_set_name(reload_dmt, name))
		goto out_reload;

	do {
		next = dm_get_next_target(dmt, next, &start, &length,
					  &target, &params);
		if (!target || !params) {
			/*
			 * We can't call dm_task_add_target() with
			 * invalid parameters. But simply dropping this
			 * target feels wrong, too. Abort and warn.
			 */
			condlog(1, "%s: invalid target found in map %s",
				__func__, name);
			goto out_reload;
		}
		buff = strdup(params);
		if (!buff) {
			condlog(3, "%s: failed to replace target %s, "
				"out of memory", name, target);
			goto out_reload;
		}
		if (strcmp(target, TGT_MPATH) && strstr(params, old)) {
			condlog(3, "%s: replace target %s %s",
				name, target, buff);
			dm_reassign_deps(buff, old, new);
			condlog(3, "%s: with target %s %s",
				name, target, buff);
			modified++;
		}
		dm_task_add_target(reload_dmt, start, length, target, buff);
		free(buff);
	} while (next);

	if (modified) {
		dm_task_no_open_count(reload_dmt);

		if (!libmp_dm_task_run(reload_dmt)) {
			dm_log_error(3, DM_DEVICE_RELOAD, reload_dmt);
			condlog(3, "%s: failed to reassign targets", name);
			goto out_reload;
		}
		dm_simplecmd_noflush(DM_DEVICE_RESUME, name,
				     MPATH_UDEV_RELOAD_FLAG);
	}
	r = 1;

out_reload:
	dm_task_destroy(reload_dmt);
out:
	dm_task_destroy(dmt);
	return r;
}


/*
 * Reassign existing device-mapper table(s) to not use
 * the block devices but point to the multipathed
 * device instead
 */
int dm_reassign(const char *mapname)
{
	struct dm_deps *deps;
	struct dm_task *dmt;
	struct dm_info info;
	char dev_t[32], dm_dep[32];
	int r = 0;
	unsigned int i;

	if (dm_dev_t(mapname, &dev_t[0], 32)) {
		condlog(3, "%s: failed to get device number", mapname);
		return 1;
	}

	if (!(dmt = libmp_dm_task_create(DM_DEVICE_DEPS))) {
		condlog(3, "%s: couldn't make dm task", mapname);
		return 0;
	}

	if (!dm_task_set_name(dmt, mapname))
		goto out;

	dm_task_no_open_count(dmt);

	if (!libmp_dm_task_run(dmt)) {
		dm_log_error(3, DM_DEVICE_DEPS, dmt);
		goto out;
	}

	if (!dm_task_get_info(dmt, &info))
		goto out;

	if (!(deps = dm_task_get_deps(dmt)))
		goto out;

	if (!info.exists)
		goto out;

	for (i = 0; i < deps->count; i++) {
		sprintf(dm_dep, "%d:%d",
			major(deps->device[i]),
			minor(deps->device[i]));
		sysfs_check_holders(dm_dep, dev_t);
	}

	r = 1;
out:
	dm_task_destroy (dmt);
	return r;
}

int dm_setgeometry(struct multipath *mpp)
{
	struct dm_task *dmt;
	struct path *pp;
	char heads[4], sectors[4];
	char cylinders[10], start[32];
	int r = 0;

	if (!mpp)
		return 1;

	pp = first_path(mpp);
	if (!pp) {
		condlog(3, "%s: no path for geometry", mpp->alias);
		return 1;
	}
	if (pp->geom.cylinders == 0 ||
	    pp->geom.heads == 0 ||
	    pp->geom.sectors == 0) {
		condlog(3, "%s: invalid geometry on %s", mpp->alias, pp->dev);
		return 1;
	}

	if (!(dmt = libmp_dm_task_create(DM_DEVICE_SET_GEOMETRY)))
		return 0;

	if (!dm_task_set_name(dmt, mpp->alias))
		goto out;

	dm_task_no_open_count(dmt);

	/* What a sick interface ... */
	snprintf(heads, 4, "%u", pp->geom.heads);
	snprintf(sectors, 4, "%u", pp->geom.sectors);
	snprintf(cylinders, 10, "%u", pp->geom.cylinders);
	snprintf(start, 32, "%lu", pp->geom.start);
	if (!dm_task_set_geometry(dmt, cylinders, heads, sectors, start)) {
		condlog(3, "%s: Failed to set geometry", mpp->alias);
		goto out;
	}

	r = libmp_dm_task_run(dmt);
	if (!r)
		dm_log_error(3, DM_DEVICE_SET_GEOMETRY, dmt);
out:
	dm_task_destroy(dmt);

	return r;
}
