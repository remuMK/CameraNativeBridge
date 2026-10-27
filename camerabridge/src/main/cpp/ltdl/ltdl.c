/* ltdl.c — minimal libltdl API shim over POSIX dlopen(). See ltdl.h.
 *
 * Details matched to what libgphoto2 actually uses:
 *   - lt_dladdsearchdir / lt_dlforeachfile: used to scan CAMLIBS / IOLIBS dirs.
 *   - lt_dlopenext: used with both bare module names ("ptp2") and full paths
 *     found by the directory scan; we try the name as-is, with ".so" appended
 *     and also a "lib"-prefixed variant (our CMake output is libptp2.so).
 */
#define _GNU_SOURCE
#include "ltdl.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <dirent.h>

#define LT_MAX_PATH 4096

static char search_path[LT_MAX_PATH] = "";
static char last_error[512] = "";
static pthread_mutex_t lt_lock = PTHREAD_MUTEX_INITIALIZER;
static int lt_init_count = 0;

static void
set_error (const char *msg)
{
	snprintf (last_error, sizeof(last_error), "%s", msg ? msg : "unknown error");
}

int
lt_dlinit (void)
{
	pthread_mutex_lock (&lt_lock);
	lt_init_count++;
	pthread_mutex_unlock (&lt_lock);
	return 0;
}

int
lt_dlexit (void)
{
	pthread_mutex_lock (&lt_lock);
	if (lt_init_count > 0)
		lt_init_count--;
	pthread_mutex_unlock (&lt_lock);
	return 0;
}

int
lt_dladdsearchdir (const char *dir)
{
	if (!dir)
		return -1;
	pthread_mutex_lock (&lt_lock);
	size_t len = strlen (search_path);
	int ok = (int) (len + strlen (dir) + 2 < LT_MAX_PATH);
	if (ok) {
		if (len)
			search_path[len++] = ':';
		strcpy (search_path + len, dir);
	}
	pthread_mutex_unlock (&lt_lock);
	return ok ? 0 : -1;
}

int
lt_dlsetsearchpath (const char *path)
{
	pthread_mutex_lock (&lt_lock);
	snprintf (search_path, sizeof(search_path), "%s", path ? path : "");
	pthread_mutex_unlock (&lt_lock);
	return 0;
}

const char *
lt_dlgetsearchpath (void)
{
	return search_path;
}

static lt_dlhandle
try_dlopen (const char *path)
{
	void *h = dlopen (path, RTLD_NOW | RTLD_LOCAL);
	if (h)
		return h;
	set_error (dlerror ());
	return NULL;
}

lt_dlhandle
lt_dlopen (const char *filename)
{
	if (!filename || !*filename)
		return NULL;
	return try_dlopen (filename);
}

lt_dlhandle
lt_dlopenext (const char *filename)
{
	if (!filename || !*filename)
		return NULL;
	char buf[LT_MAX_PATH];

	/* 1. exact name (covers full paths to ".../libptp2.so") */
	lt_dlhandle h = try_dlopen (filename);
	if (h)
		return h;

	/* 2. name + ".so" */
	snprintf (buf, sizeof(buf), "%s.so", filename);
	h = try_dlopen (buf);
	if (h)
		return h;

	/* 3. relative name resolved against the search path */
	if (filename[0] != '/') {
		/* "ptp2" -> "<searchpath>/ptp2.so" */
		snprintf (buf, sizeof(buf), "%s/%s.so", search_path, filename);
		h = try_dlopen (buf);
		if (h)
			return h;
		/* honour the "lib" prefix: "ptp2" -> "<searchpath>/libptp2.so" */
		const char *base = strrchr (filename, '/');
		base = base ? base + 1 : filename;
		snprintf (buf, sizeof(buf), "%s/lib%s.so", search_path, base);
		h = try_dlopen (buf);
		if (h)
			return h;
	}
	return NULL;
}

void *
lt_dlsym (lt_dlhandle handle, const char *name)
{
	if (!handle || !name)
		return NULL;
	void *sym = dlsym (handle, name);
	if (!sym)
		set_error (dlerror ());
	return sym;
}

int
lt_dlclose (lt_dlhandle handle)
{
	if (!handle)
		return 0;
	/* Never actually unload: camlib/iolib code contains static state and
	 * repeated gp_camera_init/gp_camera_free cycles would corrupt it.
	 * LTdl semantics forgive a leak here; on Android it is the sane choice. */
	return 0;
}

const char *
lt_dlerror (void)
{
	return last_error;
}

int
lt_dlforeachfile (const char *search_path_arg,
				  lt_dlforeachfile_callback func, void *data)
{
	if (!func)
		return -1;

	const char *paths = (search_path_arg && *search_path_arg) ?
		search_path_arg : search_path;
	char copy[LT_MAX_PATH];
	snprintf (copy, sizeof(copy), "%s", paths);

	char *saveptr = NULL;
	for (char *dir = strtok_r (copy, ":", &saveptr); dir;
		 dir = strtok_r (NULL, ":", &saveptr)) {
		DIR *d = opendir (dir);
		if (!d)
			continue;
		struct dirent *e;
		while ((e = readdir (d)) != NULL) {
#ifdef _DIRENT_HAVE_D_TYPE
			if (e->d_type == DT_DIR)
				continue;
#endif
			const char *name = e->d_name;
			size_t len = strlen (name);
			/* only consider shared objects */
			if (len > 3 && !strcmp (name + len - 3, ".so")) {
				char fullpath[LT_MAX_PATH];
				snprintf (fullpath, sizeof(fullpath), "%s/%s", dir, name);
				int r = func (fullpath, data);
				if (r != 0) {
					closedir (d);
					return r;
				}
			}
		}
		closedir (d);
	}
	return 0;
}
