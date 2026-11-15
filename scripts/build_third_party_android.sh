#!/usr/bin/env bash
# ==============================================================================
# Homography Auto-Mosaic — Reproducible Android NDK Static Third-Party Builder
# Cross-compiles:
#   1. libjpeg-turbo 3.0.4 (WITH_SIMD=ON ARM NEON, -fPIC static libturbojpeg.a)
#   2. OpenCV 4.10.0 static trimmed (core, flann, imgproc, features2d, calib3d)
#      Strictly pure CPU: OpenCL=OFF, Vulkan=OFF, TFLite/DNN=OFF, IPP=OFF
# Target ABIs: arm64-v8a, armeabi-v7a (minSdk 24 / Android 7.0+)
# ==============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
THIRD_PARTY_OUT="${PROJECT_ROOT}/app/src/main/cpp/third_party"
BUILD_WORK_DIR="${BUILD_WORK_DIR:-${PROJECT_ROOT}/.native-deps-build}"

OPENCV_VERSION="${OPENCV_VERSION:-4.10.0}"
LIBJPEG_TURBO_VERSION="${LIBJPEG_TURBO_VERSION:-3.0.4}"
ANDROID_MIN_API="${ANDROID_MIN_API:-24}"
ABIS=("arm64-v8a" "armeabi-v7a")

if [[ -z "${ANDROID_NDK_HOME:-}" ]]; then
    if [[ -n "${ANDROID_HOME:-}" && -d "${ANDROID_HOME}/ndk" ]]; then
        ANDROID_NDK_HOME="$(ls -1d "${ANDROID_HOME}/ndk/"* | sort -V | tail -n 1)"
    elif [[ -n "${ANDROID_SDK_ROOT:-}" && -d "${ANDROID_SDK_ROOT}/ndk" ]]; then
        ANDROID_NDK_HOME="$(ls -1d "${ANDROID_SDK_ROOT}/ndk/"* | sort -V | tail -n 1)"
    else
        echo "ERROR: Set ANDROID_NDK_HOME (or ANDROID_HOME with NDK installed)." >&2
        exit 1
    fi
fi

TOOLCHAIN_FILE="${ANDROID_NDK_HOME}/build/cmake/android.toolchain.cmake"
if [[ ! -f "${TOOLCHAIN_FILE}" ]]; then
    echo "ERROR: Android NDK CMake toolchain not found at ${TOOLCHAIN_FILE}" >&2
    exit 1
fi

JOBS="$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"
mkdir -p "${BUILD_WORK_DIR}" "${THIRD_PARTY_OUT}"

echo "==> Using Android NDK: ${ANDROID_NDK_HOME}"
echo "==> Output directory:  ${THIRD_PARTY_OUT}"

if [[ ! -d "${BUILD_WORK_DIR}/libjpeg-turbo" ]]; then
    git clone --depth 1 --branch "${LIBJPEG_TURBO_VERSION}" \
        https://github.com/libjpeg-turbo/libjpeg-turbo.git "${BUILD_WORK_DIR}/libjpeg-turbo"
fi

if [[ ! -d "${BUILD_WORK_DIR}/opencv" ]]; then
    git clone --depth 1 --branch "${OPENCV_VERSION}" \
        https://github.com/opencv/opencv.git "${BUILD_WORK_DIR}/opencv"
fi

for ABI in "${ABIS[@]}"; do
    echo "=================================================================="
    echo "Building libjpeg-turbo ${LIBJPEG_TURBO_VERSION} (NEON SIMD) for ${ABI}..."
    echo "=================================================================="
    JPEG_BUILD_DIR="${BUILD_WORK_DIR}/build-jpeg-${ABI}"
    JPEG_INSTALL_DIR="${THIRD_PARTY_OUT}/libjpeg-turbo/${ABI}"

    cmake -G Ninja \
        -S "${BUILD_WORK_DIR}/libjpeg-turbo" \
        -B "${JPEG_BUILD_DIR}" \
        -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN_FILE}" \
        -DANDROID_ABI="${ABI}" \
        -DANDROID_PLATFORM="android-${ANDROID_MIN_API}" \
        -DANDROID_ARM_NEON=ON \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        -DCMAKE_INSTALL_PREFIX="${JPEG_INSTALL_DIR}" \
        -DCMAKE_INSTALL_LIBDIR=lib \
        -DENABLE_STATIC=ON \
        -DENABLE_SHARED=OFF \
        -DWITH_TURBOJPEG=ON \
        -DWITH_SIMD=ON \
        -DWITH_JAVA=OFF

    cmake --build "${JPEG_BUILD_DIR}" -j "${JOBS}" --target install

    echo "=================================================================="
    echo "Building trimmed OpenCV ${OPENCV_VERSION} (Pure CPU + NEON) for ${ABI}..."
    echo "=================================================================="
    OCV_BUILD_DIR="${BUILD_WORK_DIR}/build-opencv-${ABI}"
    OCV_INSTALL_DIR="${THIRD_PARTY_OUT}/opencv/${ABI}"

    cmake -G Ninja \
        -S "${BUILD_WORK_DIR}/opencv" \
        -B "${OCV_BUILD_DIR}" \
        -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN_FILE}" \
        -DANDROID_ABI="${ABI}" \
        -DANDROID_PLATFORM="android-${ANDROID_MIN_API}" \
        -DANDROID_STL=c++_shared \
        -DANDROID_ARM_NEON=ON \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        -DCMAKE_INSTALL_PREFIX="${OCV_INSTALL_DIR}" \
        -DSDK_INSTALL_PATH="${OCV_INSTALL_DIR}" \
        -DBUILD_SHARED_LIBS=OFF \
        -DBUILD_FAT_JAVA_LIB=OFF \
        -DBUILD_ANDROID_PROJECTS=OFF \
        -DBUILD_ANDROID_EXAMPLES=OFF \
        -DBUILD_LIST=core,flann,imgproc,features2d,calib3d \
        -DBUILD_TESTS=OFF \
        -DBUILD_PERF_TESTS=OFF \
        -DBUILD_EXAMPLES=OFF \
        -DBUILD_DOCS=OFF \
        -DBUILD_JAVA=OFF \
        -DBUILD_opencv_apps=OFF \
        -DENABLE_NEON=ON \
        -DWITH_OPENCL=OFF \
        -DWITH_VULKAN=OFF \
        -DWITH_CUDA=OFF \
        -DWITH_IPP=OFF \
        -DWITH_ITT=OFF \
        -DWITH_TBB=OFF \
        -DWITH_OPENMP=OFF \
        -DWITH_EIGEN=OFF \
        -DWITH_PROTOBUF=OFF \
        -DWITH_QUIRC=OFF \
        -DWITH_ADE=OFF \
        -DWITH_JPEG=OFF \
        -DWITH_PNG=OFF \
        -DWITH_TIFF=OFF \
        -DWITH_WEBP=OFF \
        -DWITH_OPENJPEG=OFF \
        -DWITH_JASPER=OFF \
        -DWITH_OPENEXR=OFF

    cmake --build "${OCV_BUILD_DIR}" -j "${JOBS}" --target install

    # Normalize install layout if OpenCV Android CMake placed archives under sdk/native/
    if [[ -d "${OCV_INSTALL_DIR}/sdk/native" && ! -d "${OCV_INSTALL_DIR}/lib" ]]; then
        mkdir -p "${OCV_INSTALL_DIR}/lib/opencv4/3rdparty" "${OCV_INSTALL_DIR}/include"
        cp -a "${OCV_INSTALL_DIR}/sdk/native/staticlibs/${ABI}/"*.a "${OCV_INSTALL_DIR}/lib/"
        if [[ -d "${OCV_INSTALL_DIR}/sdk/native/3rdparty/libs/${ABI}" ]]; then
            cp -a "${OCV_INSTALL_DIR}/sdk/native/3rdparty/libs/${ABI}/"*.a \
                "${OCV_INSTALL_DIR}/lib/opencv4/3rdparty/" || true
        fi
        cp -a "${OCV_INSTALL_DIR}/sdk/native/jni/include/"* "${OCV_INSTALL_DIR}/include/"
    fi
done

echo "==> Successfully built Android NDK static dependencies in ${THIRD_PARTY_OUT}"
