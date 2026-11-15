cmake_minimum_required(VERSION 3.22.1)
project(homography_mosaic LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(FAM_BUILD_HOST_TESTS "Build native C++20 verification suite on host" OFF)
set(FAM_DEPS_PREFIX "" CACHE PATH "Optional prefix for host/custom OpenCV 4.10+ & libjpeg-turbo 3.0+")

# ==============================================================================
# Strict CPU-Only SIMD Compiler & Linker Flags (Red Line #1: NEON + POPCNT Only)
# ==============================================================================
set(FAM_COMMON_CXX_FLAGS
    -O3
    -DNDEBUG
    -ffast-math
    -fno-math-errno
    -funroll-loops
    -Wall
    -Wextra
)

if(ANDROID)
    list(APPEND FAM_COMMON_CXX_FLAGS -fvisibility=hidden)
    if(ANDROID_ABI STREQUAL "arm64-v8a")
        # ARMv8-A ASIMD (NEON) + hardware vector POPCNT (CNT Vd.16B, Vn.16B)
        list(APPEND FAM_COMMON_CXX_FLAGS -march=armv8-a+simd)
    elseif(ANDROID_ABI STREQUAL "armeabi-v7a")
        # ARMv7-A NEON SIMD + VCNT.8 hardware bit-count
        list(APPEND FAM_COMMON_CXX_FLAGS -march=armv7-a -mfpu=neon -mfloat-abi=softfp)
    endif()
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
    list(APPEND FAM_COMMON_CXX_FLAGS -march=armv8-a+simd)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64)$")
    # Host verification build: enable hardware POPCNT + AVX2 where available
    list(APPEND FAM_COMMON_CXX_FLAGS -mpopcnt)
endif()

# ==============================================================================
# Third-Party Static Dependencies Resolution (OpenCV 4.10+ & libjpeg-turbo 3.0+)
# ==============================================================================
set(THIRD_PARTY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party")

if(ANDROID)
    # 1. Resolve libjpeg-turbo 3.0+ static library (WITH_SIMD=ON)
    set(TURBOJPEG_ABI_DIR "${THIRD_PARTY_DIR}/libjpeg-turbo/${ANDROID_ABI}")
    set(TURBOJPEG_INC_DIR "${TURBOJPEG_ABI_DIR}/include")
    set(TURBOJPEG_STATIC_LIB "${TURBOJPEG_ABI_DIR}/lib/libturbojpeg.a")
    if(NOT EXISTS "${TURBOJPEG_INC_DIR}/turbojpeg.h")
        set(TURBOJPEG_INC_DIR "${THIRD_PARTY_DIR}/libjpeg-turbo/include")
    endif()
    if(NOT EXISTS "${TURBOJPEG_STATIC_LIB}")
        message(FATAL_ERROR
            "Missing prebuilt libjpeg-turbo 3.0+ static library at ${TURBOJPEG_STATIC_LIB}.\n"
            "Run ./scripts/build_third_party_android.sh to build OpenCV 4.10.0 + libjpeg-turbo 3.0.4.")
    endif()

    add_library(turbojpeg_static STATIC IMPORTED)
    set_target_properties(turbojpeg_static PROPERTIES
        IMPORTED_LOCATION "${TURBOJPEG_STATIC_LIB}"
        INTERFACE_INCLUDE_DIRECTORIES "${TURBOJPEG_INC_DIR}"
    )

    # 2. Resolve OpenCV 4.10+ trimmed static libraries (core, flann, imgproc, features2d, calib3d)
    set(OPENCV_CUSTOM_ABI_DIR "${THIRD_PARTY_DIR}/opencv/${ANDROID_ABI}")
    set(OPENCV_SDK_NATIVE_DIR "${THIRD_PARTY_DIR}/opencv/sdk/native")

    if(EXISTS "${OPENCV_CUSTOM_ABI_DIR}/lib/libopencv_core.a")
        set(OPENCV_INC_DIR "${OPENCV_CUSTOM_ABI_DIR}/include/opencv4")
        if(NOT EXISTS "${OPENCV_INC_DIR}")
            set(OPENCV_INC_DIR "${OPENCV_CUSTOM_ABI_DIR}/include")
        endif()
        set(OPENCV_LIB_DIR "${OPENCV_CUSTOM_ABI_DIR}/lib")
        set(OPENCV_3RDPARTY_DIR "${OPENCV_CUSTOM_ABI_DIR}/lib/opencv4/3rdparty")
    elseif(EXISTS "${OPENCV_SDK_NATIVE_DIR}/staticlibs/${ANDROID_ABI}/libopencv_core.a")
        set(OPENCV_INC_DIR "${OPENCV_SDK_NATIVE_DIR}/jni/include")
        set(OPENCV_LIB_DIR "${OPENCV_SDK_NATIVE_DIR}/staticlibs/${ANDROID_ABI}")
        set(OPENCV_3RDPARTY_DIR "${OPENCV_SDK_NATIVE_DIR}/3rdparty/libs/${ANDROID_ABI}")
    else()
        message(FATAL_ERROR
            "Missing OpenCV 4.10+ static libraries under ${THIRD_PARTY_DIR}/opencv.\n"
            "Run ./scripts/build_third_party_android.sh or unpack opencv-4.10.0-android-sdk.zip.")
    endif()

    set(OPENCV_STATIC_MODULES
        "${OPENCV_LIB_DIR}/libopencv_calib3d.a"
        "${OPENCV_LIB_DIR}/libopencv_features2d.a"
        "${OPENCV_LIB_DIR}/libopencv_imgproc.a"
        "${OPENCV_LIB_DIR}/libopencv_flann.a"
        "${OPENCV_LIB_DIR}/libopencv_core.a"
    )
    if(EXISTS "${OPENCV_3RDPARTY_DIR}")
        file(GLOB OPENCV_HAL_LIBS "${OPENCV_3RDPARTY_DIR}/lib*.a")
        list(APPEND OPENCV_STATIC_MODULES ${OPENCV_HAL_LIBS})
    endif()
    find_library(ANDROID_LOG_LIB log REQUIRED)
    find_library(ANDROID_Z_LIB z REQUIRED)
else()
    # Host C++20 verification mode
    if(FAM_DEPS_PREFIX)
        list(PREPEND CMAKE_PREFIX_PATH "${FAM_DEPS_PREFIX}")
    endif()
    find_package(JNI REQUIRED)
    find_path(TURBOJPEG_INC_DIR NAMES turbojpeg.h HINTS "${FAM_DEPS_PREFIX}/include" REQUIRED)
    find_library(TURBOJPEG_STATIC_LIB NAMES libturbojpeg.a turbojpeg HINTS "${FAM_DEPS_PREFIX}/lib" REQUIRED)
    add_library(turbojpeg_static STATIC IMPORTED)
    set_target_properties(turbojpeg_static PROPERTIES
        IMPORTED_LOCATION "${TURBOJPEG_STATIC_LIB}"
        INTERFACE_INCLUDE_DIRECTORIES "${TURBOJPEG_INC_DIR}"
    )

    find_path(OPENCV_INC_DIR NAMES opencv2/core.hpp
        HINTS "${FAM_DEPS_PREFIX}/include/opencv4" "${FAM_DEPS_PREFIX}/include" REQUIRED)
    set(OPENCV_LIB_DIR "${FAM_DEPS_PREFIX}/lib")
    set(OPENCV_STATIC_MODULES
        "${OPENCV_LIB_DIR}/libopencv_calib3d.a"
        "${OPENCV_LIB_DIR}/libopencv_features2d.a"
        "${OPENCV_LIB_DIR}/libopencv_imgproc.a"
        "${OPENCV_LIB_DIR}/libopencv_flann.a"
        "${OPENCV_LIB_DIR}/libopencv_core.a"
    )
    find_package(Threads REQUIRED)
    find_package(ZLIB QUIET)
endif()

# ==============================================================================
# Native Shared Library Target (libhomography_mosaic.so)
# ==============================================================================
add_library(homography_mosaic SHARED
    src/mosaic_engine.cpp
    src/jpeg_yuv_codec.cpp
    src/jni_bridge.cpp
)

target_include_directories(homography_mosaic
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE
        "${OPENCV_INC_DIR}"
        $<$<NOT:$<BOOL:${ANDROID}>>:${JNI_INCLUDE_DIRS}>
)

target_compile_options(homography_mosaic PRIVATE ${FAM_COMMON_CXX_FLAGS})

if(ANDROID)
    # Enforce 16 KB ELF segment alignment for Android 15+ (targetSdk 35) compatibility
    target_link_options(homography_mosaic PRIVATE
        "-Wl,-z,max-page-size=16384"
        "-Wl,--gc-sections"
        "-Wl,--exclude-libs,ALL"
    )
    target_link_libraries(homography_mosaic PRIVATE
        ${OPENCV_STATIC_MODULES}
        turbojpeg_static
        ${ANDROID_LOG_LIB}
        ${ANDROID_Z_LIB}
        dl
        m
    )
else()
    target_link_libraries(homography_mosaic PUBLIC
        ${OPENCV_STATIC_MODULES}
        turbojpeg_static
        Threads::Threads
        $<$<BOOL:${ZLIB_FOUND}>:ZLIB::ZLIB>
        dl
        m
    )
endif()

# ==============================================================================
# Optional Host End-to-End Verification & Benchmark Suite
# ==============================================================================
if(FAM_BUILD_HOST_TESTS AND NOT ANDROID)
    enable_testing()
    add_executable(test_native_pipeline
        "${CMAKE_CURRENT_SOURCE_DIR}/../../../../tests/test_native_pipeline.cpp"
    )
    target_include_directories(test_native_pipeline PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${OPENCV_INC_DIR}"
        ${JNI_INCLUDE_DIRS}
    )
    target_compile_options(test_native_pipeline PRIVATE ${FAM_COMMON_CXX_FLAGS} -Werror)
    target_link_libraries(test_native_pipeline PRIVATE homography_mosaic)
    add_test(NAME NativePipelineVerification COMMAND test_native_pipeline)
endif()
