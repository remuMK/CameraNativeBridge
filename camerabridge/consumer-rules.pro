# Keep the JNI method signatures intact.
-keepclasseswithmembernames class com.mav.cameranativebridge.** {
    native <methods>;
}
# Live-view flows and fd-passing survive R8 unmodified.
-keep class android.hardware.usb.** { *; }
-keep class java.io.FileDescriptor { *; }
