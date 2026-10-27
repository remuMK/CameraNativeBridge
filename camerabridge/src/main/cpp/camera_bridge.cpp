/*
 * camera_bridge.cpp — JNI bridge between Kotlin (NativeCameraInterface.kt)
 * and libgphoto2, cross-compiled against the Android NDK.
 *
 * Target hardware: Canon EOS M50 Mark II (PTP camera, served by the "ptp2"
 * camlib that is bundled as libptp2.so).
 *
 * ---------------------------------------------------------------------------
 * THE ANDROID USB FILE-DESCRIPTOR WORKAROUND
 * ---------------------------------------------------------------------------
 * On Android the file descriptor for a USB device comes from UsbManager:
 *
 *     UsbDeviceConnection conn = usbManager.openDevice(device);
 *     int fd = conn.getFileDescriptor();       // fd of /dev/bus/usb/…
 *
 * Android owns USB discovery and permission gating; libusb must NOT scan the
 * USB tree itself. libgphoto2 (>= 2.5.28) has first-class support for this
 * (enabled via HAVE_LIBUSB_WRAP_SYS_DEVICE / HAVE_LIBUSB_OPTION_NO_DEVICE_
 * DISCOVERY in cpp/config/config.h):
 *
 *     gp_port_usb_set_sys_device(fd);          // must happen BEFORE
 *                                              // gp_port_info_list_load()!
 *     -> the libusb1 iolib then runs
 *        libusb_set_option(ctx, LIBUSB_OPTION_NO_DEVICE_DISCOVERY)
 *        libusb_wrap_sys_device(ctx, fd, &h)
 *        and enumerates exactly the wrapped fd.
 *
 * ---------------------------------------------------------------------------
 * CAMLIB / IOLIB LOADING ON ANDROID
 * ---------------------------------------------------------------------------
 * gphoto2 dlopen()s io-libs and camera-libs from the paths in the IOLIBS and
 * CAMLIBS env vars. We point both at the app's nativeLibraryDir (passed from
 * Kotlin), where the AAR unpacked liblibusb1.so and libptp2.so. The minimal
 * libltdl shim in cpp/ltdl backs the lt_* loader API.
 *
 * ---------------------------------------------------------------------------
 * MEMORY SAFETY
 * ---------------------------------------------------------------------------
 *  - single global session, one mutex serializes ALL camera access
 *    (libgphoto2 is not thread-safe per Camera*)
 *  - gp_camera_file_read() streams downloads in bounded 256 KiB chunks
 *    instead of whole-file gp_camera_file_get() buffering -> no OOM on large
 *    RAW files
 *  - close is idempotent; failures throw typed Java exceptions
 */

#include <jni.h>
#include <android/log.h>

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <gphoto2/gphoto2.h>
#include <gphoto2/gphoto2-port.h>
#include <gphoto2/gphoto2-version.h>

#include <pthread.h>
#include <unistd.h>

#define LOG_TAG "CameraNativeBridge"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)

#define LIST_MAX_DEPTH 3               /* folder recursion depth */
#define LIST_MAX_ITEMS 2048            /* hard cap on listed files   */
#define READ_CHUNK     (256 * 1024)    /* bounded streaming chunk    */
#define PREVIEW_MAX    (64 * 1024 * 1024) /* sanity bound on LV frames */

namespace {

struct CameraSession {
	Camera    *camera  = nullptr;
	GPContext *context = nullptr;
	bool       inited  = false;
};

CameraSession    g_session;
pthread_mutex_t  g_session_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ------------------------------------------------------------------------- */
/* error / exception helpers                                                  */
/* ------------------------------------------------------------------------- */

void jniThrowImpl(JNIEnv *env, const char *cls, const char *msg)
{
	jclass ex = env->FindClass(cls);
	if (ex != nullptr) {
		env->ThrowNew(ex, msg);
		env->DeleteLocalRef(ex);
	} else { /* fall back to a plain runtime exception */
		env->ExceptionClear();
		jclass rt = env->FindClass("java/lang/RuntimeException");
		if (rt != nullptr) {
			env->ThrowNew(rt, msg);
			env->DeleteLocalRef(rt);
		}
	}
}

void jniThrowFmt(JNIEnv *env, const char *cls, const char *fmt, ...)
{
	char msg[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	jniThrowImpl(env, cls, msg);
}

/* throws with the gphoto2 result identifier attached; caller must return */
void gpThrow(JNIEnv *env, int ret, const char *what)
{
	jniThrowFmt(env, "java/io/IOException", "%s failed: %s (%d)",
				what, gp_result_as_string(ret), ret);
}

/* RAII mutex guard: libgphoto2 must never see concurrent calls. */
struct SessionGuard {
	SessionGuard(JNIEnv *env) : env(env) { pthread_mutex_lock(&g_session_mutex); }
	~SessionGuard() { pthread_mutex_unlock(&g_session_mutex); }

	Camera *cameraOrThrow()
	{
		if (g_session.camera == nullptr || !g_session.inited) {
			jniThrowImpl(env, "java/lang/IllegalStateException",
						 "camera not initialized; call resumeCamera()/init first");
			return nullptr;
		}
		return g_session.camera;
	}
	GPContext *ctx() const { return g_session.context; }

	JNIEnv *env;
};

/* checks and throws on failure (JNI exception remains pending) */
bool ok(JNIEnv *env, int ret, const char *what)
{
	if (ret < GP_OK) {
		gpThrow(env, ret, what);
		return false;
	}
	return true;
}

void setLibDirs(const char *nativeLibDir)
{
	if (nativeLibDir != nullptr && nativeLibDir[0] != '\0') {
		setenv("IOLIBS",  nativeLibDir, 1);  /* IOLIBDIR_ENV in gphoto2-port-info.h */
		setenv("CAMLIBS", nativeLibDir, 1);  /* CAMLIBS env read by abilities-list  */
	}
}

/* Selects the PTP2 camlib. Only the ptp2 camlib is shipped, but match by
 * well-known ability ids so a future multi-camlib AAR still works. */
int chooseAbilities(CameraAbilitiesList *abilities, CameraAbilities *out)
{
	int count = gp_abilities_list_count(abilities);
	if (count < GP_OK)
		return count;

	static const char *const preferred[] = {
		"PTP",
		"Canon EOS",
		"Canon",
	};

	for (const char *want : preferred) {
		size_t wantLen = strlen(want);
		for (int i = 0; i < count; ++i) {
			CameraAbilities tmp;
			if (gp_abilities_list_get_abilities(abilities, i, &tmp) != GP_OK)
				continue;
			if (strncmp(tmp.id, want, wantLen) == 0) {
				*out = tmp;
				return GP_OK;
			}
		}
	}
	/* fallback: keep whatever the single shipped camlib advertises */
	if (count > 0)
		return gp_abilities_list_get_abilities(abilities, count - 1, out);
	return GP_ERROR;
}

int findUsbPortInfo(GPPortInfoList *ports, GPPortInfo *out)
{
	int count = gp_port_info_list_count(ports);
	if (count < GP_OK)
		return count;

	for (int i = 0; i < count; ++i) {
		GPPortInfo info;
		GPPortType type = GP_PORT_NONE;
		if (gp_port_info_list_get_info(ports, i, &info) != GP_OK)
			continue;
		if (gp_port_info_get_type(info, &type) != GP_OK)
			continue;
		if (type == GP_PORT_USB) {
			*out = info;
			return GP_OK;
		}
	}
	return GP_ERROR_UNKNOWN_PORT;
}

} /* anonymous namespace */

extern "C" {

/* ------------------------------------------------------------------------- */
/* init                                                                       */
/* ------------------------------------------------------------------------- */

JNIEXPORT jboolean JNICALL
Java_com_mav_cameranativebridge_NativeCameraInterface_nativeInitWithFd(
		JNIEnv *env, jclass /*clazz*/, jint fileDescriptor, jstring nativeLibDir)
{
	if (fileDescriptor <= 0) {
		jniThrowImpl(env, "java/lang/IllegalArgumentException",
					 "fileDescriptor must be a valid UsbDeviceConnection fd");
		return JNI_FALSE;
	}
	const char *cppLibDir = nativeLibDir ? env->GetStringUTFChars(nativeLibDir, nullptr) : nullptr;

	pthread_mutex_lock(&g_session_mutex);

	if (g_session.camera != nullptr) {
		pthread_mutex_unlock(&g_session_mutex);
		if (cppLibDir) env->ReleaseStringUTFChars(nativeLibDir, cppLibDir);
		jniThrowImpl(env, "java/lang/IllegalStateException",
					 "camera already initialized; call close() first");
		return JNI_FALSE;
	}

	int ret;
	const char *failWhat = "";

	/* 1. env search paths for lt_dlopenext("libusb1"), ("ptp2") */
	setLibDirs(cppLibDir);
	if (cppLibDir) env->ReleaseStringUTFChars(nativeLibDir, cppLibDir);

	/* 2. THE FD WORKAROUND — must precede gp_port_info_list_load() */
	ret = gp_port_usb_set_sys_device(fileDescriptor);
	if (ret != GP_OK) {
		LOGE("gp_port_usb_set_sys_device(%d): %s — build lacks fd support?",
			 fileDescriptor, gp_result_as_string(ret));
	}

	/* 3. context (error/message callbacks funnel into logcat) */
	g_session.context = gp_context_new();
	gp_context_set_error_func(g_session.context,
		[](GPContext *, const char *text, void *) { LOGE("gp error: %s", text); }, nullptr);
	gp_context_set_message_func(g_session.context,
		[](GPContext *, const char *text, void *) { LOGI("gp msg: %s", text); }, nullptr);

	/* 4. camera object */
	gp_camera_new(&g_session.camera);

	/* 5. abilities (loads the camlib dlopen'ing libptp2.so) */
	CameraAbilitiesList *abilities = nullptr;
	do {
		if ((ret = gp_abilities_list_new(&abilities)) < GP_OK) { failWhat = "gp_abilities_list_new"; break; }
		if ((ret = gp_abilities_list_load(abilities, g_session.context)) < GP_OK) { failWhat = "gp_abilities_list_load"; break; }

		CameraAbilities apt;
		if ((ret = chooseAbilities(abilities, &apt)) < GP_OK) { failWhat = "no usable camlib"; break; }
		if ((ret = gp_camera_set_abilities(g_session.camera, apt)) < GP_OK) { failWhat = "gp_camera_set_abilities"; break; }
	} while (false);
	if (abilities) gp_abilities_list_free(abilities);
	if (ret < GP_OK) goto fail;

	/* 6. port list (loads iolib dlopen'ing liblibusb1.so; sees only the fd) */
	{
		GPPortInfoList *ports = nullptr;
		if ((ret = gp_port_info_list_new(&ports)) < GP_OK) { failWhat = "gp_port_info_list_new"; goto fail; }
		if ((ret = gp_port_info_list_load(ports)) < GP_OK) { failWhat = "gp_port_info_list_load"; gp_port_info_list_free(ports); goto fail; }

		GPPortInfo chosen = nullptr;
		if ((ret = findUsbPortInfo(ports, &chosen)) < GP_OK) {
			failWhat = "no USB port (iolib load failed?)";
			gp_port_info_list_free(ports);
			goto fail;
		}
		if ((ret = gp_camera_set_port_info(g_session.camera, chosen)) < GP_OK) {
			failWhat = "gp_camera_set_port";
			gp_port_info_list_free(ports);
			goto fail;
		}
		gp_port_info_list_free(ports);
	}

	/* 7. connect (PTP open over the wrapped fd — UsbManager owns claim/perm) */
	if ((ret = gp_camera_init(g_session.camera, g_session.context)) < GP_OK) {
		failWhat = "gp_camera_init";
		goto fail;
	}

	g_session.inited = true;
	LOGI("camera session initialized (fd=%d)", fileDescriptor);
	pthread_mutex_unlock(&g_session_mutex);
	return JNI_TRUE;

fail:
	LOGE("%s: %s (%d)", failWhat, gp_result_as_string(ret), ret);
	gp_camera_free(g_session.camera);
	g_session.camera  = nullptr;
	g_session.inited  = false;
	gp_context_unref(g_session.context);
	g_session.context = nullptr;
	gp_port_usb_set_sys_device(-1);
	pthread_mutex_unlock(&g_session_mutex);
	gpThrow(env, ret, failWhat);
	return JNI_FALSE;
}

/* ------------------------------------------------------------------------- */
/* live view frame (MJPEG/JPEG body of a single PTP preview object)           */
/* ------------------------------------------------------------------------- */

JNIEXPORT jbyteArray JNICALL
Java_com_mav_cameranativebridge_NativeCameraInterface_nativeGetPreviewFrame(
		JNIEnv *env, jclass /*clazz*/)
{
	SessionGuard guard(env);
	Camera *camera = guard.cameraOrThrow();
	if (camera == nullptr)
		return nullptr;

	CameraFile *file = nullptr;
	if (gp_file_new(&file) != GP_OK) {
		jniThrowImpl(env, "java/io/IOException", "gp_file_new failed");
		return nullptr;
	}

	int ret = gp_camera_capture_preview(camera, file, guard.ctx());
	if (ret < GP_OK) {
		gp_file_unref(file);
		gpThrow(env, ret, "gp_camera_capture_preview");
		return nullptr;
	}

	const char *data = nullptr;
	unsigned long size = 0;
	if (gp_file_get_data_and_size(file, &data, &size) != GP_OK) {
		gp_file_unref(file);
		jniThrowImpl(env, "java/io/IOException", "gp_file_get_data_and_size failed");
		return nullptr;
	}

	jbyteArray out = nullptr;
	if (size == 0 || size > PREVIEW_MAX) {
		LOGW("preview frame size %lu: dropped", size);
		jniThrowImpl(env, "java/io/IOException", "preview frame empty or oversized");
	} else {
		out = env->NewByteArray(static_cast<jsize>(size));
		if (out != nullptr)
			env->SetByteArrayRegion(out, 0, static_cast<jsize>(size),
									reinterpret_cast<const jbyte *>(data));
	}
	gp_file_unref(file);
	return out;
}

/* ------------------------------------------------------------------------- */
/* file list: "<folder-path><file>" entries, depth-limited recursion          */
/* ------------------------------------------------------------------------- */

void collectTree(JNIEnv *env, Camera *camera, GPContext *ctx,
				 const char *folder, int depth, jobjectArray outline,
				 jint *idx, jint maxEntries)
{
	if (depth > LIST_MAX_DEPTH || *idx >= maxEntries)
		return;

	CameraList *files = nullptr;
	if (gp_list_new(&files) == GP_OK) {
		if (gp_camera_folder_list_files(camera, folder, files, ctx) == GP_OK) {
			int n = gp_list_count(files);
			for (int i = 0; i < n && *idx < maxEntries; ++i) {
				const char *name = nullptr;
				if (gp_list_get_name(files, i, &name) != GP_OK || !name)
					continue;
				char full[1024];
				snprintf(full, sizeof(full), "%s%s", folder, name);
				jstring jstr = env->NewStringUTF(full);
				if (jstr) {
					env->SetObjectArrayElement(outline, (*idx)++, jstr);
					env->DeleteLocalRef(jstr);
				}
			}
		}
		gp_list_free(files);
	}

	CameraList *dirs = nullptr;
	if (gp_list_new(&dirs) == GP_OK) {
		if (gp_camera_folder_list_folders(camera, folder, dirs, ctx) == GP_OK) {
			int n = gp_list_count(dirs);
			for (int i = 0; i < n && *idx < maxEntries; ++i) {
				const char *name = nullptr;
				if (gp_list_get_name(dirs, i, &name) != GP_OK || !name)
					continue;
				char sub[1024];
				if (strcmp(folder, "/") == 0)
					snprintf(sub, sizeof(sub), "/%s/", name);
				else
					snprintf(sub, sizeof(sub), "%s%s/", folder, name);
				collectTree(env, camera, ctx, sub, depth + 1, outline, idx, maxEntries);
			}
		}
		gp_list_free(dirs);
	}
}

JNIEXPORT jobjectArray JNICALL
Java_com_mav_cameranativebridge_NativeCameraInterface_nativeGetFileList(
		JNIEnv *env, jclass /*clazz*/)
{
	SessionGuard guard(env);
	Camera *camera = guard.cameraOrThrow();
	if (camera == nullptr)
		return nullptr;

	jclass stringClass = env->FindClass("java/lang/String");
	jobjectArray buf =
		env->NewObjectArray(LIST_MAX_ITEMS, stringClass, nullptr);
	if (buf == nullptr)
		return nullptr; /* OOM pending */

	jint idx = 0;
	collectTree(env, camera, guard.ctx(), "/", 0, buf, &idx, LIST_MAX_ITEMS);

	jobjectArray exact = env->NewObjectArray(idx, stringClass, nullptr);
	for (jint i = 0; i < idx; ++i) {
		jstring s = static_cast<jstring>(env->GetObjectArrayElement(buf, i));
		env->SetObjectArrayElement(exact, i, s);
		env->DeleteLocalRef(s);
	}
	env->DeleteLocalRef(buf);
	return exact;
}

/* ------------------------------------------------------------------------- */
/* streaming download to a caller-owned fd (OOM-safe via bounded chunks)      */
/* ------------------------------------------------------------------------- */

JNIEXPORT void JNICALL
Java_com_mav_cameranativebridge_NativeCameraInterface_nativeDownloadFileTo(
		JNIEnv *env, jclass /*clazz*/, jstring folder, jstring fileName, jint outFd)
{
	SessionGuard guard(env);
	Camera *camera = guard.cameraOrThrow();
	if (camera == nullptr)
		return;

	const char *cFolder = env->GetStringUTFChars(folder, nullptr);
	const char *cFile   = env->GetStringUTFChars(fileName, nullptr);
	if (cFolder == nullptr || cFile == nullptr) {
		if (cFolder) env->ReleaseStringUTFChars(folder, cFolder);
		if (cFile)   env->ReleaseStringUTFChars(fileName, cFile);
		jniThrowImpl(env, "java/lang/IllegalArgumentException", "path args required");
		return;
	}

	int fd = (outFd > 0) ? ::dup(outFd) : -1;
	if (fd < 0) {
		env->ReleaseStringUTFChars(folder, cFolder);
		env->ReleaseStringUTFChars(fileName, cFile);
		jniThrowImpl(env, "java/io/IOException", "cannot dup output fd");
		return;
	}

	char *chunk = static_cast<char *>(malloc(READ_CHUNK));
	if (chunk == nullptr) {
		::close(fd);
		env->ReleaseStringUTFChars(folder, cFolder);
		env->ReleaseStringUTFChars(fileName, cFile);
		jniThrowImpl(env, "java/io/IOException", "out of memory (stream buffer)");
		return;
	}

	bool okFlag = true;
	uint64_t offset = 0;
	for (;;) {
		uint64_t size = READ_CHUNK;
		int ret = gp_camera_file_read(camera, cFolder, cFile,
									  GP_FILE_TYPE_NORMAL,
									  offset, chunk, &size, guard.ctx());
		if (ret < GP_OK) {
			LOGE("file_read at off %llu: %s (%d)",
				 (unsigned long long) offset, gp_result_as_string(ret), ret);
			okFlag = false;
			break;
		}
		if (size == 0) break; /* EOF */

		uint64_t written = 0;
		while (written < size) {
			ssize_t n = ::write(fd, chunk + written, (size_t) (size - written));
			if (n < 0) {
				LOGE("write(): errno=%d", errno);
				okFlag = false;
				break;
			}
			written += (uint64_t) n;
		}
		if (!okFlag)
			break;
		offset += size;
	}

	::close(fd);       /* our dup only — caller's ParcelFileDescriptor lives */
	free(chunk);
	env->ReleaseStringUTFChars(folder, cFolder);
	env->ReleaseStringUTFChars(fileName, cFile);

	if (!okFlag)
		jniThrowImpl(env, "java/io/IOException", "download aborted (see logcat)");
}

/* ------------------------------------------------------------------------- */
/* graceful, idempotent shutdown                                              */
/* ------------------------------------------------------------------------- */

JNIEXPORT void JNICALL
Java_com_mav_cameranativebridge_NativeCameraInterface_nativeClose(
		JNIEnv * /*env*/, jclass /*clazz*/)
{
	pthread_mutex_lock(&g_session_mutex);

	if (g_session.camera != nullptr && g_session.inited)
		gp_camera_exit(g_session.camera, g_session.context);
	g_session.inited = false;

	if (g_session.camera != nullptr) {
		gp_camera_free(g_session.camera);
		g_session.camera = nullptr;
	}
	if (g_session.context != nullptr) {
		gp_context_unref(g_session.context);
		g_session.context = nullptr;
	}
	gp_port_usb_set_sys_device(-1); /* forget the UsbManager fd */

	pthread_mutex_unlock(&g_session_mutex);
	LOGI("camera session closed");
}

/* ------------------------------------------------------------------------- */
/* diagnostic: linked libgphoto2 version string                               */
/* ------------------------------------------------------------------------- */

JNIEXPORT jstring JNICALL
Java_com_mav_cameranativebridge_NativeCameraInterface_nativeVersion(
		JNIEnv *env, jclass /*clazz*/)
{
	const char **ver = gp_library_version(GP_VERSION_SHORT);
	char buf[256] = "libgphoto2 <unknown>";
	if (ver != nullptr && ver[0] != nullptr)
		snprintf(buf, sizeof(buf), "libgphoto2 %s %s",
				 ver[0], ver[1] != nullptr ? ver[1] : "");
	return env->NewStringUTF(buf);
}

} /* extern "C" */
