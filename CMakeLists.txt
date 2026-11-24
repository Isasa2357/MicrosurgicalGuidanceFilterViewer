cmake_minimum_required(VERSION 3.21)

project(MicrosurgicalGuidanceFilterViewer LANGUAGES C CXX)

if(NOT WIN32)
    message(FATAL_ERROR "This D3D12 viewer requires Windows and the Windows SDK")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Download and build OpenCV as part of this CMake project.
# No vcpkg installation or externally installed OpenCV is required.
include(FetchContent)

# Keep the tested 4.x API and pin the dependency to a specific release.
set(_opencv_version "4.14.0")

# Only build what this viewer needs (and OpenCV's required dependencies).
# Static OpenCV modules avoid opencv_*.dll runtime dependencies.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build OpenCV as static libraries" FORCE)
set(BUILD_LIST "core,imgproc,videoio" CACHE STRING "OpenCV modules" FORCE)
set(BUILD_opencv_world OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_PERF_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_opencv_apps OFF CACHE BOOL "" FORCE)
set(BUILD_JAVA OFF CACHE BOOL "" FORCE)
set(BUILD_opencv_python2 OFF CACHE BOOL "" FORCE)
set(BUILD_opencv_python3 OFF CACHE BOOL "" FORCE)
set(WITH_IPP OFF CACHE BOOL "Avoid Intel IPP download" FORCE)
set(WITH_ITT OFF CACHE BOOL "" FORCE)
set(WITH_OPENCL OFF CACHE BOOL "" FORCE)
set(WITH_CUDA OFF CACHE BOOL "" FORCE)
set(OPENCV_GENERATE_SETUPVARS OFF CACHE BOOL "" FORCE)
set(OPENCV_GENERATE_PKGCONFIG OFF CACHE BOOL "" FORCE)

# On Windows, OpenCV downloads the matching prebuilt FFmpeg backend
# at configure time. It is required to decode raw .h264 recordings.
set(WITH_FFMPEG ON CACHE BOOL "Use OpenCV's FFmpeg video backend" FORCE)
set(OPENCV_FFMPEG_USE_FIND_PACKAGE OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    opencv
    GIT_REPOSITORY https://github.com/opencv/opencv.git
    GIT_TAG        ${_opencv_version}
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE
)
FetchContent_MakeAvailable(opencv)

foreach(_module IN ITEMS core imgproc videoio)
    if(NOT TARGET opencv_${_module})
        message(FATAL_ERROR
            "OpenCV module opencv_${_module} was not built. "
            "Check the OpenCV configuration output.")
    endif()
endforeach()

add_executable(MicrosurgicalGuidanceFilterViewer
    main.cpp
    src/D3D12StereoRenderer.hpp
    src/D3D12StereoRenderer.cpp
)

target_compile_definitions(MicrosurgicalGuidanceFilterViewer PRIVATE
    NOMINMAX WIN32_LEAN_AND_MEAN
)

# Runtime HLSL compilation uses native Windows D3DCompileFromFile.
# No third-party graphics helper, DirectXTK or CD3DX12 headers.
add_custom_command(
    TARGET MicrosurgicalGuidanceFilterViewer POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders"
        "$<TARGET_FILE_DIR:MicrosurgicalGuidanceFilterViewer>/shaders"
    COMMENT "Copying D3D12 HLSL shaders beside the executable"
    VERBATIM
)

# OpenCV builds with the static MSVC runtime (/MT, /MTd) in this configuration.
# Match that CRT on the viewer executable, otherwise linking fails with LNK2038.
# In particular, using BUILD_SHARED_LIBS=OFF does not automatically set /MT
# on the consumer executable.
if(MSVC)
    set_property(TARGET MicrosurgicalGuidanceFilterViewer PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
endif()

# OpenCV sets a global EXECUTABLE_OUTPUT_PATH under <build>/bin.
# Override it for our viewer so documented paths remain <build>/Release/*.exe.
set_target_properties(MicrosurgicalGuidanceFilterViewer PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# OpenCV's build-tree module targets use PRIVATE include paths, so consumers
# in a parent FetchContent project must explicitly include public module headers.
# OPENCV_CONFIG_FILE_INCLUDE_DIR points to the build-generated headers,
# including opencv2/opencv_modules.hpp (normally the top-level build directory).
if(NOT EXISTS "${OPENCV_CONFIG_FILE_INCLUDE_DIR}/opencv2/opencv_modules.hpp")
    message(FATAL_ERROR
        "OpenCV generated header was not found: "
        "${OPENCV_CONFIG_FILE_INCLUDE_DIR}/opencv2/opencv_modules.hpp")
endif()

target_include_directories(MicrosurgicalGuidanceFilterViewer SYSTEM PRIVATE
    "${opencv_SOURCE_DIR}/include"
    "${OPENCV_CONFIG_FILE_INCLUDE_DIR}"
    "${opencv_SOURCE_DIR}/modules/core/include"
    "${opencv_SOURCE_DIR}/modules/imgproc/include"
    "${opencv_SOURCE_DIR}/modules/imgcodecs/include"
    "${opencv_SOURCE_DIR}/modules/videoio/include"
)

target_link_libraries(MicrosurgicalGuidanceFilterViewer PRIVATE
    opencv_core
    opencv_imgproc
    opencv_videoio
    d3d12
    dxgi
    d3dcompiler
    dxguid
    user32
    gdi32
)

if(WIN32)
    # A static OpenCV build still needs its FFmpeg backend DLL at runtime.
    # The OpenCV 4.14.0 configure step downloads this to its build tree.
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(_ffmpeg_arch_suffix "_64")
    else()
        set(_ffmpeg_arch_suffix "")
    endif()

    string(REPLACE "." "" _opencv_version_digits "${_opencv_version}")
    set(_ffmpeg_downloaded
        "${opencv_BINARY_DIR}/3rdparty/ffmpeg/opencv_videoio_ffmpeg${_ffmpeg_arch_suffix}.dll")
    set(_ffmpeg_runtime_name
        "opencv_videoio_ffmpeg${_opencv_version_digits}${_ffmpeg_arch_suffix}.dll")

    if(NOT EXISTS "${_ffmpeg_downloaded}")
        message(FATAL_ERROR
            "OpenCV FFmpeg backend DLL was not downloaded: ${_ffmpeg_downloaded}\n"
            "OpenCV's Windows build downloads this from opencv/opencv_3rdparty.\n"
            "Check Internet access and the FFMPEG entries in the CMake configure log.")
    endif()

    add_custom_command(
        TARGET MicrosurgicalGuidanceFilterViewer POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${_ffmpeg_downloaded}"
            "$<TARGET_FILE_DIR:MicrosurgicalGuidanceFilterViewer>/${_ffmpeg_runtime_name}"
        COMMENT "Copying OpenCV's FFmpeg backend for raw H.264 playback"
        VERBATIM
    )
endif()

if(MSVC)
    target_compile_options(MicrosurgicalGuidanceFilterViewer PRIVATE /W4)
else()
    target_compile_options(MicrosurgicalGuidanceFilterViewer PRIVATE -Wall -Wextra)
endif()
