plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.android)
    alias(libs.plugins.kotlin.compose)
}

val customKeystorePath = System.getenv("FAM_KEYSTORE_PATH")
    ?: File(System.getProperty("user.home"), ".gemini/homography-mosaic-release.jks")
        .takeIf { it.isFile }?.absolutePath
val customKeystoreFile = customKeystorePath?.let { File(it) }?.takeIf { it.isFile }

android {
    namespace = "com.antigravity.homographymosaic"
    compileSdk = 35
    ndkVersion = System.getenv("FAM_NDK_VERSION")?.takeIf { it.isNotBlank() } ?: "27.0.12077973"

    defaultConfig {
        applicationId = "com.antigravity.homographymosaic"
        minSdk = 24
        targetSdk = 35
        versionCode = 225
        versionName = "2.2.5-FAMTPL02"

        ndk {
            // Strict cross-vendor ARMv8 & ARMv7 NEON support
            abiFilters += listOf("arm64-v8a", "armeabi-v7a")
        }

        externalNativeBuild {
            cmake {
                cppFlags += listOf("-std=c++20", "-O3")
                arguments += listOf(
                    "-DANDROID_STL=c++_shared",
                    "-DANDROID_ARM_NEON=ON",
                    "-DANDROID_PLATFORM=android-24"
                )
            }
        }
    }

    signingConfigs {
        if (customKeystoreFile != null) {
            create("releaseCustom") {
                storeFile = customKeystoreFile
                storePassword = System.getenv("FAM_KEYSTORE_PASS") ?: "AutoMosaic2026!"
                keyAlias = System.getenv("FAM_KEY_ALIAS") ?: "homographymosaic"
                keyPassword = System.getenv("FAM_KEY_PASS") ?: "AutoMosaic2026!"
                enableV1Signing = true
                enableV2Signing = true
                enableV3Signing = true
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = true
            isShrinkResources = true
            signingConfig = if (customKeystoreFile != null) {
                signingConfigs.getByName("releaseCustom")
            } else {
                signingConfigs.getByName("debug")
            }
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
        }
        debug {
            isMinifyEnabled = false
        }
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

    buildFeatures {
        compose = true
        buildConfig = true
    }

    packaging {
        jniLibs {
            // Supports Android 15+ (targetSdk 35) 16 KB page-aligned uncompressed .so loading
            useLegacyPackaging = false
        }
        resources {
            excludes += "/META-INF/{AL2.0,LGPL2.1}"
        }
    }
}

afterEvaluate {
    tasks.matching { it.name == "assembleRelease" }.configureEach {
        doLast {
            val releaseDir = layout.buildDirectory.dir("outputs/apk/release").get().asFile
            val builtApk = releaseDir.listFiles()
                ?.firstOrNull { it.isFile && it.extension == "apk" && it.name != "自碼系統.apk" }
            if (builtApk != null) {
                val namedApk = File(releaseDir, "自碼系統.apk")
                builtApk.copyTo(namedApk, overwrite = true)
            }
        }
    }
}

dependencies {
    implementation(libs.androidx.core.ktx)
    implementation(libs.androidx.lifecycle.runtime.ktx)
    implementation(libs.androidx.lifecycle.runtime.compose)
    implementation(libs.androidx.activity.compose)
    implementation(libs.kotlinx.coroutines.android)

    implementation(platform(libs.androidx.compose.bom))
    implementation(libs.androidx.ui)
    implementation(libs.androidx.ui.graphics)
    implementation(libs.androidx.ui.tooling.preview)
    implementation(libs.androidx.material3)
    implementation(libs.androidx.material.icons.extended)
}
