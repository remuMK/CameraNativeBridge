#!/usr/bin/env bash
# Fetch (or link) the vendored C sources needed by the CameraNativeBridge build.
#
#   cpp/vendor/libgphoto2   https://github.com/gphoto/libgphoto2
#   cpp/vendor/libusb       https://github.com/libusb/libusb  (1.0.27+ required
#                           for LIBUSB_OPTION_NO_DEVICE_DISCOVERY +
#                           libusb_wrap_sys_device used by the Android fd path)
#
# You can also point the variables below at an existing checkout and the script
# will simply symlink instead of cloning.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VENDOR="$ROOT/camerabridge/src/main/cpp/vendor"
mkdir -p "$VENDOR"

LIBGPHOTO2_SRC="${LIBGPHOTO2_SRC:-$VENDOR/libgphoto2}"
LIBUSB_SRC="${LIBUSB_SRC:-$VENDOR/libusb}"

if [[ -e "$LIBGPHOTO2_SRC" ]]; then
    echo "libgphoto2 source present: $LIBGPHOTO2_SRC"
else
    git clone --depth 1 https://github.com/gphoto/libgphoto2 "$LIBGPHOTO2_SRC"
fi

if [[ -e "$LIBUSB_SRC" ]]; then
    echo "libusb source present: $LIBUSB_SRC"
else
    git clone --depth 1 --branch v1.0.27 https://github.com/libusb/libusb "$LIBUSB_SRC"
fi

# For Cable/UsbManager to work with libusb on Android, libusb must be >= 1.0.23.
echo "Done."
