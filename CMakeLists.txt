cmake_minimum_required(VERSION 3.28)
set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type")
project(gzport C CXX RC)

if(NOT CMAKE_C_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR "Use clang (sysv_abi and GNU inline assembly): cmake -G Ninja -DCMAKE_C_COMPILER=clang")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
option(GZ_DISTRIBUTION_BUILD "Omit local build paths and debug records from distributable binaries" OFF)
if(GZ_DISTRIBUTION_BUILD)
    add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:-ffile-prefix-map=${CMAKE_SOURCE_DIR}=.>"
                        "$<$<COMPILE_LANGUAGE:C,CXX>:-fdebug-prefix-map=${CMAKE_SOURCE_DIR}=.>"
                        "$<$<COMPILE_LANGUAGE:C,CXX>:-fdebug-compilation-dir=.>")
endif()

# WPF frontend; .NET Framework is supplied by Windows, no build tools at launch.
add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/Godzilla.exe"
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File
        "${CMAKE_SOURCE_DIR}/tools/build_launcher.ps1" -Output "${CMAKE_BINARY_DIR}/Godzilla.exe"
    DEPENDS src/GodzillaLauncher.cs src/LauncherLanguages.cs src/ReleaseUpdater.cs src/PromptArtworkUpdate.cs src/NativeRuntime.cs src/GodzillaUpdater.cs src/OwnerIcon.cs src/godzilla.ico tools/build_launcher.ps1
    VERBATIM)
add_custom_target(Godzilla ALL DEPENDS "${CMAKE_BINARY_DIR}/Godzilla.exe")

add_executable(gzport
    src/loader.c
    src/rt_core.c
    src/rt_sync.c
    src/rt_thread.c
    src/rt_memory.c
    src/rt_vmem.c
    src/rt_sema.c
    src/rt_services.c
    src/rt_file.c
    src/rt_savedata.c
    src/rt_kernel.c
    src/rt_rtc.c
)
target_sources(gzport PRIVATE src/godzilla.rc src/app_icon.cpp)
target_compile_options(gzport PRIVATE -Wall -Wextra)
target_sources(gzport PRIVATE src/dialog_ui.c)
target_sources(gzport PRIVATE src/snapshot_view.c)
target_link_libraries(gzport PRIVATE user32 gdi32 gdiplus)
target_compile_definitions(gzport PRIVATE WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
if(GZ_DISTRIBUTION_BUILD)
    target_link_options(gzport PRIVATE -Xlinker /debug:none)
endif()

option(GZ_GPU "Build the shadPS4 Vulkan graphics backend" ON)
if(GZ_GPU)
    add_subdirectory(gpu)
    target_link_libraries(gzport PRIVATE gzgpu)
    target_compile_definitions(gzport PRIVATE GZ_GPU=1)
endif()
