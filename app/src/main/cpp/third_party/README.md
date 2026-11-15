# Native Third-Party Static Libraries (`third_party/`)

This directory serves as the mount point for the pure-CPU static native libraries required by `app/src/main/cpp/CMakeLists.txt`:

1. **`libjpeg-turbo` 3.0.4+** (`WITH_SIMD=ON` ARM NEON `tjDecompressToYUVPlanes` / `tjCompressFromYUVPlanes`)
2. **`OpenCV` 4.10.0+** static trimmed modules (`core`, `flann`, `imgproc`, `features2d`, `calib3d`) built with `WITH_OPENCL=OFF`, `WITH_VULKAN=OFF`, and `ENABLE_NEON=ON`.

## Expected Directory Layout

### Automated Build (Recommended)
From the repository root, run:

```bash
chmod +x scripts/build_third_party_android.sh
ANDROID_NDK_HOME=/path/to/android-ndk ./scripts/build_third_party_android.sh
```

This cross-compiles both `arm64-v8a` and `armeabi-v7a` (`minSdk 24`) with `-fPIC` and populates:

```text
third_party/
├── libjpeg-turbo/
│   ├── arm64-v8a/
│   │   ├── include/turbojpeg.h
│   │   └── lib/libturbojpeg.a
│   └── armeabi-v7a/
│       ├── include/turbojpeg.h
│       └── lib/libturbojpeg.a
└── opencv/
    ├── arm64-v8a/
    │   ├── include/opencv4/opencv2/...
    │   └── lib/libopencv_{core,flann,imgproc,features2d,calib3d}.a
    └── armeabi-v7a/
        ├── include/opencv4/opencv2/...
        └── lib/libopencv_{core,flann,imgproc,features2d,calib3d}.a
```

### Alternative Official OpenCV Android SDK Mount
`CMakeLists.txt` also automatically detects the official `opencv-4.10.0-android-sdk.zip` layout if unpacked directly into `third_party/opencv/sdk/native/`.
