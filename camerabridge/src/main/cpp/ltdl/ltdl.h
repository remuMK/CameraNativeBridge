/* ltdl.h — a minimal, Android-friendly "libltdl" API shim.
 *
 * Upstream libgphoto2 dynamically loads its io-libs and camera-libs through the
 * libltdl API (lt_dlopenext & friends). We do not ship libltdl; instead a thin
 * wrapper over the POSIX dl* API implemented in ltdl.c, plus a writable search
 * path. At runtime camera_bridge.cpp points the search path at the AAR's
 * nativeLibraryDir (via the IOLIBS / CAMLIBS env vars that gphoto2 already
 * supports).
 */
#ifndef ANDROID_LTDL_SHIM_H
#define ANDROID_LTDL_SHIM_H

#include <stddef.h>

typedef void *lt_dlhandle;

extern int      lt_dlinit (void);
extern int      lt_dlexit (void);
extern int      lt_dladdsearchdir (const char *search_dir);
extern int      lt_dlsetsearchpath (const char *search_path);
extern const char *lt_dlgetsearchpath (void);

extern lt_dlhandle lt_dlopen (const char *filename);
extern lt_dlhandle lt_dlopenext (const char *filename);
extern void   *lt_dlsym (lt_dlhandle handle, const char *name);
extern int     lt_dlclose (lt_dlhandle handle);
extern const char *lt_dlerror (void);

typedef int (*lt_dlforeachfile_callback)(const char *filename, void *data);
extern int     lt_dlforeachfile (const char *search_path,
                                 lt_dlforeachfile_callback func, void *data);

/* libltdl type kept for upstream source compatibility */
typedef void *lt_ptr;

#endif /* ANDROID_LTDL_SHIM_H */
