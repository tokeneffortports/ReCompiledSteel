# Real Steel - static recompilation (ReXGlue) with a native Vulkan renderer.
#
# Inputs the build needs from YOUR copy of the game (see docs/building.md):
#   work/game/   the extracted Real Steel package (default.xex, default.xexp, pac/, ...)
#   work/tu/     the extracted title update package (pac/)
# The recompiled code (generated/default_final/) and the shader cache
# (generated/shader_cache.cpp) are produced from them at build time and are
# never checked in.

cmake_minimum_required(VERSION 3.25)
project(realsteel LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# No build-machine paths in the binaries (__FILE__ in log lines, debug
# info): the source tree and whatever sits beside it (the SDK mirror on
# Linux) are spelled relative.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    get_filename_component(RS_PARENT_DIR "${CMAKE_CURRENT_SOURCE_DIR}" DIRECTORY)
    add_compile_options("-ffile-prefix-map=${CMAKE_CURRENT_SOURCE_DIR}=."
                        "-ffile-prefix-map=${RS_PARENT_DIR}=..")
endif()

# ReXGlue SDK boilerplate (written by `rexglue init`): finds the SDK, runs
# codegen on realsteel_manifest.toml, defines rexglue_setup_target().
# It only lists the recompiled sources that exist when configuring, so on a
# fresh tree codegen is run here first (needs work/game/default.xex +
# default.xexp; a few seconds).
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/generated/default_final/sources.cmake"
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/work/game/default.xex")
    if(REXSDK_DIR)
        message(WARNING "First configure with REXSDK_DIR (SDK source tree): run "
                        "'rexglue codegen realsteel_manifest.toml' once, then configure again.")
    else()
        if(REXSDK_VERSION)
            find_package(rexglue ${REXSDK_VERSION} EXACT QUIET CONFIG)
        else()
            find_package(rexglue 0.10.0 QUIET CONFIG)
        endif()
        set(RS_REXGLUE_EXE "${RS_HOST_REXGLUE}")
        if(NOT RS_REXGLUE_EXE AND TARGET rex::rexglue)
            foreach(prop IMPORTED_LOCATION IMPORTED_LOCATION_RELEASE IMPORTED_LOCATION_RELWITHDEBINFO
                         IMPORTED_LOCATION_NOCONFIG IMPORTED_LOCATION_DEBUG)
                get_target_property(candidate rex::rexglue ${prop})
                if(candidate AND EXISTS "${candidate}")
                    set(RS_REXGLUE_EXE "${candidate}")
                    break()
                endif()
            endforeach()
        endif()
        if(NOT RS_REXGLUE_EXE AND rexglue_DIR)
            find_program(RS_REXGLUE_EXE NAMES rexglue PATHS "${rexglue_DIR}/../../../bin" NO_DEFAULT_PATH)
        endif()
        if(RS_REXGLUE_EXE)
            message(STATUS "First configure: generating the recompiled code with ${RS_REXGLUE_EXE}")
            execute_process(COMMAND "${RS_REXGLUE_EXE}" codegen realsteel_manifest.toml
                            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
                            RESULT_VARIABLE rs_codegen_result)
            if(NOT rs_codegen_result EQUAL 0)
                message(FATAL_ERROR "rexglue codegen failed (${rs_codegen_result})")
            endif()
        endif()
    endif()
endif()
include(generated/rexglue.cmake)
# Cross builds (Android): code generation runs on the build machine, so the
# codegen step uses the host SDK's rexglue (RS_HOST_REXGLUE) instead of the
# target package's.
set(RS_HOST_REXGLUE "" CACHE FILEPATH "Host rexglue executable for cross builds")
if(RS_HOST_REXGLUE AND TARGET rex::rexglue)
    foreach(cfg "" _RELEASE _RELWITHDEBINFO _DEBUG _NOCONFIG)
        set_target_properties(rex::rexglue PROPERTIES IMPORTED_LOCATION${cfg} "${RS_HOST_REXGLUE}")
    endforeach()
endif()

# ---------------------------------------------------------------------------
# Game inputs
# ---------------------------------------------------------------------------
set(RS_GAME_DIR "${CMAKE_CURRENT_SOURCE_DIR}/work/game" CACHE PATH
    "Extracted Real Steel package: default.xex, default.xexp, pac/")
set(RS_TU_DIR "${CMAKE_CURRENT_SOURCE_DIR}/work/tu" CACHE PATH
    "Extracted title update package: pac/ (optional)")
if(NOT EXISTS "${RS_GAME_DIR}/default.xex")
    message(FATAL_ERROR
        "Game files not found: ${RS_GAME_DIR}/default.xex\n"
        "Put your own copy of the game in work/ first - see docs/building.md "
        "(tools/pac/stfs_extract.py --work work <game package> <title update package>).")
endif()

# ---------------------------------------------------------------------------
# Shader cache: the game's shaders translated to SPIR-V by the Real Steel
# fork of XenosRecomp (tools/shaders/build_shader_cache.py).
# ---------------------------------------------------------------------------
set(RS_XENOSRECOMP "" CACHE FILEPATH
    "XenosRecomp executable built from thirdparty/XenosRecomp (see docs/building.md)")
set(RS_SHADER_CACHE "${CMAKE_CURRENT_SOURCE_DIR}/generated/shader_cache.cpp")
if(NOT RS_XENOSRECOMP)
    find_program(RS_XENOSRECOMP_FOUND NAMES XenosRecomp
        PATHS "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/XenosRecomp/build/XenosRecomp"
              "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/XenosRecomp/build/XenosRecomp/Release"
        NO_DEFAULT_PATH)
    if(RS_XENOSRECOMP_FOUND)
        set(RS_XENOSRECOMP "${RS_XENOSRECOMP_FOUND}")
    endif()
endif()
find_package(Python3 COMPONENTS Interpreter QUIET)
if(RS_XENOSRECOMP AND Python3_Interpreter_FOUND)
    set(RS_TU_ARGS "")
    if(EXISTS "${RS_TU_DIR}/pac")
        set(RS_TU_ARGS --tu "${RS_TU_DIR}")
    endif()
    add_custom_command(
        OUTPUT "${RS_SHADER_CACHE}"
        COMMAND "${Python3_EXECUTABLE}" tools/shaders/build_shader_cache.py
                --game "${RS_GAME_DIR}" ${RS_TU_ARGS}
                --xenosrecomp "${RS_XENOSRECOMP}" --out "${RS_SHADER_CACHE}"
        DEPENDS tools/shaders/build_shader_cache.py tools/pac/extract_shaders.py
                tools/pac/yukes_bpe.py thirdparty/XenosRecomp-realsteel.patch
                "${RS_XENOSRECOMP}"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        COMMENT "Translating the game's shaders (generated/shader_cache.cpp)"
        VERBATIM)
    message(STATUS "Shader cache: generated with ${RS_XENOSRECOMP}")
elseif(EXISTS "${RS_SHADER_CACHE}")
    message(STATUS "Shader cache: using the existing ${RS_SHADER_CACHE} "
                   "(set RS_XENOSRECOMP to regenerate it)")
else()
    message(FATAL_ERROR
        "No shader cache and no XenosRecomp: build the Real Steel fork of "
        "XenosRecomp (tools/shaders/setup_xenosrecomp.py) or pass "
        "-DRS_XENOSRECOMP=<path to XenosRecomp executable>; Python 3 must be "
        "installed. See docs/building.md.")
endif()

# ---------------------------------------------------------------------------
# Third-party libraries, downloaded by the build (none are kept in this
# repository): the Vulkan headers and loader, and the two libraries that read
# the shader cache - zstd (decompression) and smol-v (SPIR-V decoding), at the
# commits XenosRecomp pins. thirdparty/ only holds our patches.
# ---------------------------------------------------------------------------
include(FetchContent)
set(VULKAN_HEADERS_ENABLE_MODULE OFF CACHE BOOL "" FORCE)
FetchContent_Declare(VulkanHeaders
    GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers
    GIT_TAG v1.3.296)
FetchContent_Declare(volk
    GIT_REPOSITORY https://github.com/zeux/volk
    GIT_TAG 1.3.295)
# Sources only: SOURCE_SUBDIR names a folder without a CMakeLists.txt, so
# nothing of their own build is added.
FetchContent_Declare(zstd
    GIT_REPOSITORY https://github.com/facebook/zstd
    GIT_TAG f7a8bb1263448e5028aceeba606a08fe3809550f  # v1.5.7
    SOURCE_SUBDIR sources-only)
FetchContent_Declare(smolv
    GIT_REPOSITORY https://github.com/aras-p/smol-v
    GIT_TAG 9dd54c379ac29fa148cb1b829bb939ba7381d8f4
    SOURCE_SUBDIR sources-only)
FetchContent_MakeAvailable(VulkanHeaders volk zstd smolv)
target_include_directories(volk PUBLIC "${vulkanheaders_SOURCE_DIR}/include")
if(WIN32)
    target_compile_definitions(volk PUBLIC VK_USE_PLATFORM_WIN32_KHR)
elseif(ANDROID)
    target_compile_definitions(volk PUBLIC VK_USE_PLATFORM_ANDROID_KHR)
endif()
file(GLOB RS_ZSTD_DEC_SOURCES
    "${zstd_SOURCE_DIR}/lib/common/*.c"
    "${zstd_SOURCE_DIR}/lib/decompress/*.c")
add_library(rs_zstd_dec STATIC ${RS_ZSTD_DEC_SOURCES})
target_include_directories(rs_zstd_dec PUBLIC "${zstd_SOURCE_DIR}/lib")
target_compile_definitions(rs_zstd_dec PRIVATE ZSTD_DISABLE_ASM)
add_library(rs_smolv STATIC "${smolv_SOURCE_DIR}/source/smolv.cpp")
target_include_directories(rs_smolv PUBLIC "${smolv_SOURCE_DIR}/source")

# ---------------------------------------------------------------------------
# The game
# ---------------------------------------------------------------------------
set(REALSTEEL_SOURCES
    src/main.cpp
    src/realsteel_gamma_dump.cpp
    src/realsteel_gpu.cpp
    src/realsteel_mods.cpp
    src/realsteel_mods_pack.cpp
    src/realsteel_netplay.cpp
    src/realsteel_online.cpp
    src/realsteel_overlay.cpp
    src/realsteel_perf.cpp
    src/realsteel_touch.cpp
    src/realsteel_ui_text.cpp
    src/realsteel_native_settings.cpp
    src/realsteel_aspect.cpp
    src/realsteel_hooks.cpp
    src/realsteel_keyboard.cpp
    src/realsteel_host_window.cpp
    src/realsteel_native_present.cpp
    src/realsteel_native_render.cpp
    src/realsteel_shader_dump.cpp
    src/realsteel_texture_dump.cpp
    # The native renderer is the offline replayer's backend, compiled in
    tools/replayer/capture_format.cpp
    tools/replayer/texture_decode.cpp
    tools/replayer/shader_library.cpp
    tools/replayer/vk_backend.cpp
    "${RS_SHADER_CACHE}"
)

if(WIN32)
    add_executable(realsteel WIN32 ${REALSTEEL_SOURCES})
elseif(ANDROID)
    # SDL's Java activity loads libmain.so and calls SDL_main in it
    # (android/, see android/README.md).
    add_library(realsteel SHARED ${REALSTEEL_SOURCES})
    set_target_properties(realsteel PROPERTIES OUTPUT_NAME main)
else()
    add_executable(realsteel ${REALSTEEL_SOURCES})
endif()

if(ANDROID)
    # The built-in GPU service replaces the emulation plugin.
    rexglue_setup_target(realsteel)
    target_link_libraries(realsteel PRIVATE android log)
else()
    rexglue_setup_target(realsteel GPU_PLUGINS xenos)
endif()

# The executable's own icon (Explorer, taskbar): a resource script compiled
# with llvm-rc (or rc.exe) when one is around; the window icon is embedded
# in code either way (src/realsteel_icon.h).
if(WIN32)
    find_program(RS_RC_COMPILER NAMES llvm-rc rc)
    if(RS_RC_COMPILER)
        set(CMAKE_RC_COMPILER "${RS_RC_COMPILER}")
        enable_language(RC)
        target_sources(realsteel PRIVATE src/realsteel.rc)
    endif()
endif()

# Linker map: resolves the module+offset stacks the GPU service logs when the
# guest stalls; recompiled functions appear as sub_XXXXXXXX.
if(WIN32)
    target_link_options(realsteel PRIVATE "LINKER:/MAP:${CMAKE_BINARY_DIR}/realsteel.map")
endif()
# xxhash.h comes with the ReXGlue SDK (its include folder).
target_include_directories(realsteel PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/replayer")
target_link_libraries(realsteel PRIVATE volk rs_zstd_dec rs_smolv)
if(WIN32)
    target_link_libraries(realsteel PRIVATE ws2_32)  # realsteel_netplay.cpp
endif()

# ---------------------------------------------------------------------------
# The setup program players run to copy their game files in (tools/setup)
# ---------------------------------------------------------------------------
if(NOT ANDROID)
    add_subdirectory(tools/setup)
endif()
