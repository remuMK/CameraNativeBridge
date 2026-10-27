# =============================================================================
# CameraNativeBridge — cross-compilation of libusb / libgphoto2_port /
# libgphoto2 (+ the PTP2 camlib that powers Canon EOS M50 Mark II) for Android.
#
# Expected layout:
#   cpp/vendor/libgphoto2   <- full gphoto2 source tree (git clone or submodule)
#   cpp/vendor/libusb       <- full libusb-1.0 source tree (git clone/submodule)
#
# The upstream build system (meson/autotools) is not used on Android. Instead
# this CMake file compiles the relevant source files directly, using:
#   - a hand-written Android config.h            (cpp/config/config.h)
#   - a hand-written gphoto2-endian.h            (cpp/config/gphoto2-endian.h)
#   - a small dlopen() based "libltdl" shim so that the iolibs (liblibusb1.so)
#     and the camlib (libptp2.so) can still be loaded dynamically at runtime
#     from the app's nativeLibraryDir (pointed at via the IOLIBS/CAMLIBS env
#     vars which libgphoto2 supports out of the box).
#
# Shared libraries shipped in the AAR:
#   libcamera_bridge.so  - the JNI layer (what Kotlin loads)
#   libgphoto2_port.so   - port core
#   libusb1.so           - the libusb-1.0 iolib  (dlopen'ed at runtime)
#   libgphoto2.so        - gphoto2 core
#   libptp2.so           - the PTP camlib        (dlopen'ed at runtime)
#   libusb core is linked statically into libgphoto2_port.so / libusb1.so
#
# Have no config.h in your way? Everything the Android port needs is below.
# =============================================================================

cmake_minimum_required(VERSION 3.22.1)
project(camera_bridge LANGUAGES C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(VENDOR_DIR ${CMAKE_CURRENT_SOURCE_DIR}/vendor)
set(GP_DIR     ${VENDOR_DIR}/libgphoto2)
set(LUSB_DIR   ${VENDOR_DIR}/libusb)

if(NOT EXISTS ${GP_DIR}/libgphoto2_port)
    message(WARNING "cpp/vendor/libgphoto2 missing — run scripts/fetch-sources.sh")
endif()
if(NOT EXISTS ${LUSB_DIR}/libusb)
    message(WARNING "cpp/vendor/libusb missing — run scripts/fetch-sources.sh")
endif()

# Common include path layout (upstream expects these)
set(GP_INC ${GP_DIR})
set(PORT_INC ${GP_DIR}/libgphoto2_port)
set(CONFIG_INC ${CMAKE_CURRENT_SOURCE_DIR}/config)
set(LTDL_INC ${CMAKE_CURRENT_SOURCE_DIR}/ltdl)

# -----------------------------------------------------------------------------
# Global compile definitions for the Android, NDK, no-NLS, minimal deps build
# -----------------------------------------------------------------------------
add_library(gp_compile_defs INTERFACE)
target_compile_definitions(gp_compile_defs INTERFACE
    HAVE_CONFIG_H
    # ptp2 / libgphoto2 misc guards
    HAVE_LSTAT
    HAVE_CFMAKERAW
    HAVE_SELECT
    HAVE_SYS_TIME_H
    HAVE_SYS_PARAM_H
    HAVE_UNISTD_H
    HAVE_STDLIB_H
    HAVE_STRING_H
    HAVE_STRINGS_H
    HAVE_STDINT_H
    HAVE_INTTYPES_H
    HAVE_MEMMEM
    HAVE_VA_COPY
    # internal-code guard required to compile upstream internals (GP_LOG_E etc.)
    _GPHOTO2_INTERNAL_CODE
    # ---- USB / Android fd workaround ------------------------------------------
    # HAVE_LIBUSB_WRAP_SYS_DEVICE, HAVE_LIBUSB_OPTION_NO_DEVICE_DISCOVERY and
    # HAVE_LIBUSB1 are defined in cpp/config/config.h together with the
    # comments explaining the fd flow. Do not re-define them here.
    # ---- The things we deliberately DISABLE on Android ------------------------
    # no gettext, no iconv, no libxml2, no libjpeg, no libexif, no old libusb-0.1
    # serial port, disk port, usbscsi, usbdiskdirect, vusb are not compiled.
)
target_include_directories(gp_compile_defs INTERFACE
    ${CONFIG_INC}
    ${LTDL_INC}
    ${GP_INC}
    ${GP_INC}/libgphoto2
    ${PORT_INC}
    ${PORT_INC}/gphoto2
    ${LUSB_DIR}/libusb
)

# armeabi-v7a needs unaligned-safe access, NDK handles this, but make sure we
# do not get thumb/NEON surprises from third-party code:
if(DEFINED CACHE{ANDROID_ABI} AND "$CACHE{ANDROID_ABI}" STREQUAL "armeabi-v7a")
    target_compile_options(gp_compile_defs INTERFACE -march=armv7-a -mfloat-abi=softfp -mfpu=neon)
endif()

# -----------------------------------------------------------------------------
# 1) libltdl shim (dlopen based) — required for iolib/camlib loading
# -----------------------------------------------------------------------------
add_library(ltdl STATIC ltdl/ltdl.c)
target_include_directories(ltdl PUBLIC ${LTDL_INC})
target_include_directories(ltdl PUBLIC SYSTEM ${GP_INC} ${PORT_INC}/gphoto2 ${CONFIG_INC})
target_compile_definitions(ltdl PUBLIC HAVE_CONFIG_H)
target_compile_definitions(ltdl PRIVATE INCLUDED_LIBLTDL=1)
target_link_libraries(ltdl PUBLIC gp_compile_defs)

# -----------------------------------------------------------------------------
# 2) libusb-1.0 (static; core only, no linux_udev — netlink hotplug instead)
# -----------------------------------------------------------------------------
set(LUSB_CORE
    ${LUSB_DIR}/libusb/core.c
    ${LUSB_DIR}/libusb/descriptor.c
    ${LUSB_DIR}/libusb/hotplug.c
    ${LUSB_DIR}/libusb/io.c
    ${LUSB_DIR}/libusb/sync.c
    ${LUSB_DIR}/libusb/strerror.c
    ${LUSB_DIR}/libusb/os/linux_usbfs.c
    ${LUSB_DIR}/libusb/os/linux_netlink.c
    ${LUSB_DIR}/libusb/os/threads_posix.c
)
# events backend file was renamed across libusb versions; pick what exists
if(EXISTS ${LUSB_DIR}/libusb/os/events_posix.c)
    list(APPEND LUSB_CORE ${LUSB_DIR}/libusb/os/events_posix.c)
endif()
if(EXISTS ${LUSB_DIR}/libusb/os/events_linux.c)
    list(APPEND LUSB_CORE ${LUSB_DIR}/libusb/os/events_linux.c)
endif()
if(EXISTS ${LUSB_DIR}/libusb/os/events_common.c)
    list(APPEND LUSB_CORE ${LUSB_DIR}/libusb/os/events_common.c)
endif()

add_library(usb1_static STATIC ${LUSB_CORE})
# libusb ships an android/config.h tailored for NDK builds — use it outright
target_compile_definitions(usb1_static PRIVATE HAVE_CONFIG_H)
target_include_directories(usb1_static PRIVATE BEFORE ${LUSB_DIR}/android)
target_include_directories(usb1_static PUBLIC ${LUSB_DIR}/libusb ${LUSB_DIR})
target_compile_options(usb1_static PRIVATE -Wno-deprecated-declarations)
target_link_libraries(usb1_static PUBLIC gp_compile_defs)

# -----------------------------------------------------------------------------
# 3) libgphoto2_port — core + the libusb-1.0 iolib
# -----------------------------------------------------------------------------
set(PORT_CORE
    ${PORT_INC}/libgphoto2_port/gphoto2-port.c
    ${PORT_INC}/libgphoto2_port/gphoto2-port-info-list.c
    ${PORT_INC}/libgphoto2_port/gphoto2-port-locking.c
    ${PORT_INC}/libgphoto2_port/gphoto2-port-log.c
    ${PORT_INC}/libgphoto2_port/gphoto2-port-portability.c
    ${PORT_INC}/libgphoto2_port/gphoto2-port-result.c
    ${PORT_INC}/libgphoto2_port/gphoto2-port-version.c
)

add_library(gphoto2_port SHARED ${PORT_CORE})
target_compile_definitions(gphoto2_port PRIVATE
    _GNU_SOURCE
    AM_H=1
    URL_USB_MASSSTORAGE="http://www.linux-usb.org/USB-guide/x498.html"
)
target_link_libraries(gphoto2_port PUBLIC ltdl)
if(ANDROID)
target_link_libraries(gphoto2_port INTERFACE log)
endif()

target_include_directories(gphoto2_port PRIVATE ${GP_INC} ${PORT_INC})
target_link_libraries(gphoto2_port PUBLIC gp_compile_defs)

# The libusb-1.0 iolib — loaded at runtime via lt_dlopen(".../liblibusb1.so")
add_library(usb1 SHARED ${PORT_INC}/libusb1/libusb1.c)
target_compile_definitions(usb1 PRIVATE
    URL_USB_MASSSTORAGE="http://www.linux-usb.org/USB-guide/x498.html")
target_link_libraries(usb1 PRIVATE gphoto2_port usb1_static)
target_link_libraries(usb1 PUBLIC gp_compile_defs)

# -----------------------------------------------------------------------------
# 4) libgphoto2 core (+ PTP2 camlib for the Canon EOS M50 Mark II)
# -----------------------------------------------------------------------------
set(GP_CORE
    ${GP_INC}/libgphoto2/gphoto2-abilities-list.c
    ${GP_INC}/libgphoto2/gphoto2-camera.c
    ${GP_INC}/libgphoto2/gphoto2-context.c
    ${GP_INC}/libgphoto2/gphoto2-file.c
    ${GP_INC}/libgphoto2/gphoto2-filesys.c
    ${GP_INC}/libgphoto2/gphoto2-list.c
    ${GP_INC}/libgphoto2/gphoto2-result.c
    ${GP_INC}/libgphoto2/gphoto2-setting.c
    ${GP_INC}/libgphoto2/gphoto2-version.c
    ${GP_INC}/libgphoto2/gphoto2-widget.c
    # BAYER / JPEG preview decoding helpers (used by capture_preview paths)
    ${GP_INC}/libgphoto2/ahd_bayer.c
    ${GP_INC}/libgphoto2/bayer.c
    ${GP_INC}/libgphoto2/exif.c
    ${GP_INC}/libgphoto2/gamma.c
    ${GP_INC}/libgphoto2/jpeg.c
)

add_library(gphoto2 SHARED ${GP_CORE})
target_compile_definitions(gphoto2 PRIVATE _GNU_SOURCE)
target_link_libraries(gphoto2 PUBLIC gphoto2_port)
target_link_libraries(gphoto2 PUBLIC gp_compile_defs)

set(PTP2_DIR ${GP_INC}/camlibs/ptp2)
set(PTP2_SRC
    ${PTP2_DIR}/ptp.c
    ${PTP2_DIR}/library.c
    ${PTP2_DIR}/usb.c
    ${PTP2_DIR}/ptpip.c
    ${PTP2_DIR}/config.c
    ${PTP2_DIR}/olympus-wrap.c
    ${PTP2_DIR}/chdk.c
    ${PTP2_DIR}/fujiptpip.c
    ${PTP2_DIR}/eos-lv-histogram.c
)

add_library(ptp2 SHARED ${PTP2_SRC})
target_compile_definitions(ptp2 PRIVATE _GNU_SOURCE)
target_link_libraries(ptp2 PUBLIC gphoto2 gphoto2_port)
target_link_libraries(ptp2 PUBLIC gp_compile_defs)

# -----------------------------------------------------------------------------
# 5) the JNI bridge (Android only)
# -----------------------------------------------------------------------------
if(ANDROID)
add_library(camera_bridge SHARED camera_bridge.cpp)

target_compile_features(camera_bridge PRIVATE cxx_std_17)
target_link_libraries(camera_bridge PRIVATE gphoto2 gphoto2_port ptp2 android log)
target_link_libraries(camera_bridge PUBLIC gp_compile_defs)
else()
add_library(camera_bridge EXCLUDE_FROM_ALL INTERFACE)
message(STATUS "Not ANDROID: skipping camera_bridge JNI target (host smoke test build)")
endif()

# -----------------------------------------------------------------------------
# warnings hygiene — upstream source produces a LOT of warnings; keep the build
# log readable
# -----------------------------------------------------------------------------
foreach(tgt gphoto2 gphoto2_port ptp2 usb1 usb1_static ltdl)
    target_compile_options(${tgt} PRIVATE
        -Wno-unused-parameter -Wno-sign-compare -Wno-unused-function
        -Wno-deprecated-declarations)
endforeach()
