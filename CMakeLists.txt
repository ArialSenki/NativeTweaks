cmake_minimum_required(VERSION 3.24)

project(NativeTweaks LANGUAGES CXX)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "NativeTweaks requires a 64-bit Windows toolchain.")
endif()

# One DLL for English, Spanish, Simplified and Traditional Chinese: the menu
# follows the game language unless [General] Language in NativeTweaks.ini forces one.
add_library(NativeTweaks MODULE src/native_tweaks.cpp src/vanilla_settings.cpp src/hotkeys.cpp
    src/game_memory.cpp src/traditional_chinese.cpp src/live_text.cpp src/perf_log.cpp
    src/spirit_ashes.cpp)
target_include_directories(NativeTweaks PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ERNativeUI/include")
target_compile_features(NativeTweaks PRIVATE cxx_std_17)
target_compile_definitions(NativeTweaks PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
set_target_properties(NativeTweaks PROPERTIES
    PREFIX ""
    OUTPUT_NAME "NativeTweaks")

if(MSVC)
    target_compile_options(NativeTweaks PRIVATE /W4 /permissive- /utf-8)
    # MSVC 14.40+ makes std::mutex constexpr, which an older MSVCP140.dll
    # (common in Proton/Wine prefixes) cannot lock: keep the old constructor.
    target_compile_definitions(NativeTweaks PRIVATE _DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR)
endif()

# Multiple Spirit Ashes (Rust, msa\) is linked in as a static library; cargo
# decides itself whether anything needs rebuilding. The libraries after it are
# the ones rustc reports with --print native-static-libs.
set(NT_MSA_TARGET_DIR "$ENV{LOCALAPPDATA}/NativeTweaks-msa-build" CACHE PATH
    "Cargo target directory for the built-in Multiple Spirit Ashes library")
set(NT_MSA_LIB "${NT_MSA_TARGET_DIR}/x86_64-pc-windows-msvc/release/multiple_spirit_ashes.lib")
file(TO_NATIVE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/msa/build.cmd" NT_MSA_BUILD)
file(TO_NATIVE_PATH "${NT_MSA_TARGET_DIR}" NT_MSA_TARGET_DIR_NATIVE)
add_custom_target(nt_msa
    COMMAND ${CMAKE_COMMAND} -E env "NT_MSA_TARGET_DIR=${NT_MSA_TARGET_DIR_NATIVE}" cmd /c "${NT_MSA_BUILD}"
    BYPRODUCTS "${NT_MSA_LIB}"
    COMMENT "Building Multiple Spirit Ashes (msa)"
    VERBATIM)
add_dependencies(NativeTweaks nt_msa)
target_link_libraries(NativeTweaks PRIVATE "${NT_MSA_LIB}" ntdll userenv ws2_32 dbghelp)
