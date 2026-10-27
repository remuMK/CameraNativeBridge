package com.mav.cameranativebridge

import android.content.Context
import android.hardware.usb.UsbConstants
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbDeviceConnection
import android.hardware.usb.UsbManager
import android.os.ParcelFileDescriptor
import android.util.Log
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.flow
import kotlinx.coroutines.flow.flowOn
import kotlinx.coroutines.withContext
import java.io.File

/**
 * Kotlin facade over libgphoto2, cross-compiled for Android by
 * [CameraNativeBridge] (`camerabridge` module, AAR output).
 *
 * Target camera: **Canon EOS M50 Mark II** (PTP protocol, served by the
 * `ptp2` camlib shipped in the AAR as `libptp2.so`).
 *
 * How the USB file descriptor workaround works
 * --------------------------------------------
 * Android's [UsbManager] owns USB device discovery and permission gating, so
 * libusb may not enumerate devices itself. We open the device through
 * UsbManager and pass the raw fd over JNI:
 *
 * ```
 * UsbDeviceConnection conn = usbManager.openDevice(device);
 * int fd = conn.fileDescriptor;                 // /dev/bus/usb/xxx/yyy
 * ```
 *
 * The C side calls `gp_port_usb_set_sys_device(fd)` *before* any port-list
 * load; the `usb1` iolib then wraps the fd with
 * `libusb_wrap_sys_device(...)` and skips USB discovery entirely
 * (`LIBUSB_OPTION_NO_DEVICE_DISCOVERY`). Only the wrapped device is ever
 * seen by libgphoto2. (Upstream: gphoto2-port.c, libgphoto2_port/libusb1.)
 *
 * The `.so` runtime files (`liblibusb1.so`, `libptp2.so`) are unpacked next
 * to the JNI lib; `nativeLibraryDir` is handed to libgphoto2 as both its
 * `IOLIBS` and `CAMLIBS` directory, and the libltdl shim dlopen()s them.
 */
class NativeCameraInterface(private val appContext: Context) : AutoCloseable {

    companion object {
        private const val TAG = "NativeCameraInterface"
        private const val PREVIEW_INTERVAL_MS = 100L

        /** Canon, Inc. */
        const val CANON_VENDOR_ID = 0x04A9

        /** Live view frame cadence: USB 2.0 realistically gives ~6-10 fps. */
        const val MAX_PREVIEW_FPS = 10

        @Volatile private var applicationsLoaded = false

        @JvmStatic
        fun loadNative() {
            if (!applicationsLoaded) {
                System.loadLibrary("camera_bridge")
                applicationsLoaded = true
            }
        }
    }

    init { loadNative() }

    /* --------------------------------------------------------------- */
    /* native functions (JNI)                                          */
    /* --------------------------------------------------------------- */

    private external fun nativeInitWithFd(fileDescriptor: Int, nativeLibDir: String): Boolean
    private external fun nativeGetPreviewFrame(): ByteArray?
    private external fun nativeGetFileList(): Array<String>
    private external fun nativeDownloadFileTo(folder: String, fileName: String, outFd: Int)
    private external fun nativeClose()
    private external fun nativeVersion(): String

    /* --------------------------------------------------------------- */
    /* lifecycle                                                       */
    /* --------------------------------------------------------------- */

    @Volatile private var opened = false

    /** Linked libgphoto2 version, for diagnostics. */
    fun libraryVersion(): String = nativeVersion()

    /**
     * Initializes a libgphoto2 session over an existing UsbManager-owned
     * connection. Throws SecurityException if no USB permission was granted
     * and IOException for anything gphoto-side.
     */
    @Synchronized
    fun init(connection: UsbDeviceConnection): NativeCameraInterface {
        check(!opened) { "already initialized; call close() first" }
        val fd = connection.fileDescriptor
        require(fd > 0) { "connection.fileDescriptor invalid" }
        val libDir = appContext.applicationInfo.nativeLibraryDir
        if (!nativeInitWithFd(fd, libDir)) {
            Log.e(TAG, "nativeInitWithFd failed")
            error("camera init failed — check logcat for gp_camera_init details")
        }
        opened = true
        Log.i(TAG, "camera session opened (fd=$fd); ${nativeVersion()}")
        return this
    }

    /** Convenience: opens the device if permission is already granted. */
    fun init(usbManager: UsbManager, device: UsbDevice): NativeCameraInterface {
        check(usbManager.hasPermission(device)) {
            "USB permission not granted for $device — request it from the UI layer first"
        }
        return init(usbManager.openDevice(device)
            ?: error("openDevice() returned null"))
    }

    @Synchronized
    override fun close() {   /* AutoCloseable.use()-friendly */
        if (opened) {
            runCatching { nativeClose() }
            opened = false
            Log.i(TAG, "camera session closed")
        }
    }

    /* --------------------------------------------------------------- */
    /* live view                                                       */
    /* --------------------------------------------------------------- */

    /**
     * One-shot capture_preview frame (JPEG/MJPEG body).
     * @throws IllegalStateException if closed
     * @throws IOException if the camera refused / timed out
     */
    fun capturePreviewOnce(): ByteArray {
        check(opened) { "camera is closed" }
        return nativeGetPreviewFrame()
            ?: error("no preview frame — camera may not support live view now")
    }

    /**
     * Continuous live-view frames. Safe to share via collect on IO dispatcher.
     * Stays cool: bounded by USB throughput, does not spin if the camera
     * stalls (emits only when a frame bytes-buffer is non-empty).
     */
    fun previewFlow(intervalMs: Long = PREVIEW_INTERVAL_MS): Flow<ByteArray> =
        flow {
            while (opened) {
                nativeGetPreviewFrame()?.let { emit(it) }
                delay(intervalMs)
            }
        }.flowOn(Dispatchers.IO)

    /* --------------------------------------------------------------- */
    /* storage / files                                                 */
    /* --------------------------------------------------------------- */

    /**
     * Lists content paths as "<folder><file>" strings, e.g.
     * `/store_00010001/DCIM/100CANON/IMG_0001.JPG`. Recursion is depth-limited.
     */
    suspend fun listFiles(): Array<String> {
        check(opened) { "camera is closed" }
        return withContext(Dispatchers.IO) { nativeGetFileList() }
    }

    /**
     * Streams a photo to disk using `gp_camera_file_read` in bounded ~256 KiB
     * chunks — large .CR3 RAWs cannot OOM the process.
     */
    @Synchronized
    fun downloadFileTo(folder: String, fileName: String, destination: File) {
        check(opened) { "camera is closed" }
        ParcelFileDescriptor.open(
            destination,
            ParcelFileDescriptor.MODE_CREATE or
                ParcelFileDescriptor.MODE_TRUNCATE or
                ParcelFileDescriptor.MODE_WRITE_ONLY,
        ).use { pfd ->
            nativeDownloadFileTo(folder, fileName, pfd.fd)
        }
    }

    /** Streaming variant for pipes / sinks that emulate a raw fd. */
    @Synchronized
    fun downloadFileTo(folder: String, fileName: String, sink: ParcelFileDescriptor) {
        check(opened) { "camera is closed" }
        nativeDownloadFileTo(folder, fileName, sink.fd)
    }

    /* --------------------------------------------------------------- */
    /* helpers                                                         */
    /* --------------------------------------------------------------- */

    /**
     * True for Canon PTP imaging devices (EOS M50 Mark II et al.).
     * Note: bionic/UsbConstants spells the still-image class "CSCID" (class 0x06).
     */
    fun isCanonM50(device: UsbDevice): Boolean {
        if (device.vendorId != CANON_VENDOR_ID) return false
        if (device.deviceClass == UsbConstants.USB_CLASS_CSCID) return true
        for (i in 0 until device.interfaceCount) {
            if (device.getInterface(i).interfaceClass == UsbConstants.USB_CLASS_CSCID) {
                return true
            }
        }
        return false
    }

    @Deprecated("use close()")
    fun shutdown() = close()
}
