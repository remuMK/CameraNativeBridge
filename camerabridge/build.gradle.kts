plugins {
    id("com.android.library")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "com.mav.cameranativebridge"
    compileSdk = 34

    defaultConfig {
        minSdk = 24          // camera_bridge + libgphoto2 need nothing newer
        consumerProguardFiles("consumer-rules.pro")

        // AGP downloads this NDK on demand (licenses auto-accepted on CI).
        ndkVersion = "27.0.12077973"

        ndk {
            abiFilters.addAll(listOf("arm64-v8a", "armeabi-v7a"))
        }

        externalNativeBuild {
            cmake {
                cppFlags += listOf(
                    "-std=c++17",
                    "-fexceptions",       // (JNI exception handling)
                    "-frtti",
                )
                arguments += listOf(
                    "-DCMAKE_BUILD_TYPE=Release",
                    "-DANDROID_STL=c++_static",  // keep the AAR small: one less .so
                )
            }
        }

        // bionic memcpy/strdup quirks show up as noise during builds
        lint { abortOnError = false }
    }

    /*
     * The 5 shared libraries produced by CMake are all packaged:
     *   libcamera_bridge.so   (JNI layer)
     *   libgphoto2.so / libgphoto2_port.so
     *   libptp2.so / liblibusb1.so   <- dlopen()-ed at runtime
     * "useLegacyPackaging = true" makes sure they get extracted from the APK
     * to a real directory (nativeLibraryDir) where dlopen can find them.
     */
    packaging {
        jniLibs.useLegacyPackaging = true
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions {
        jvmTarget = "17"
    }
}

dependencies {
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.8.1")
}
