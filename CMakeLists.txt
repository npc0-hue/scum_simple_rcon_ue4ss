cmake_minimum_required(VERSION 3.20)

set(TARGET scum_simple_rcon)
project(${TARGET} LANGUAGES CXX)

add_library(${TARGET} SHARED
    src/dllmain.cpp
    src/SimpleRconMod.cpp
    src/RconServer.cpp
    src/CommandQueue.cpp
    src/ScumBridge.cpp
    src/OfflineAdminExecutor.cpp
    src/RealChatDispatcher.cpp
)

target_compile_features(${TARGET} PRIVATE cxx_std_20)
target_include_directories(${TARGET} PRIVATE include)

target_compile_definitions(${TARGET} PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WINSOCK_DEPRECATED_NO_WARNINGS
    # The UE4SS headers pick platform and member-layout headers from these,
    # matching the Development Win64 game build the mod is loaded by. The
    # profiler and STATS switches are deliberately left off: they only add
    # instrumentation this mod does not use, and they pull in Tracy.
    UE_GAME
    UE_BUILD_DEVELOPMENT
    _UNICODE
    UNICODE
    _WIN32_WINNT=0x0A00
    WINVER=0x0A00
    PLATFORM_WINDOWS
    PLATFORM_MICROSOFT
    OVERRIDE_PLATFORM_HEADER_NAME=Windows
    UBT_COMPILED_PLATFORM=Win64
)

if(MSVC)
    # /utf-8 is required by fmt, which the UE4SS headers pull in through
    # DynamicOutput; it refuses to compile without it.
    #
    # /permissive- is deliberately not set. fmt's chrono.h holds a std::locale
    # in an anonymous union (detail::get_locale), which conforming mode reports
    # as the errors C4582/C4583 instead of warnings, and RE-UE4SS itself builds
    # these same headers without conformance mode.
    target_compile_options(${TARGET} PRIVATE /EHsc /bigobj /utf-8)
endif()

# The mod only needs UE4SS's public headers. Every UE4SS symbol it uses is
# imported from UE4SS.dll at load time, so building the UE4SS project itself
# (and its Rust dependencies) is not required. Pass -DUE4SS_SOURCE_DIR=/path/to/RE-UE4SS.
if(NOT DEFINED UE4SS_SOURCE_DIR)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../RE-UE4SS/UE4SS/include/UE4SSRuntime.hpp")
        set(UE4SS_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../RE-UE4SS")
    else()
        message(FATAL_ERROR
            "UE4SS headers not found. Pass -DUE4SS_SOURCE_DIR=/path/to/RE-UE4SS, "
            "or place this project next to a RE-UE4SS checkout.")
    endif()
endif()

if(NOT EXISTS "${UE4SS_SOURCE_DIR}/UE4SS/include/UE4SSRuntime.hpp")
    message(FATAL_ERROR "UE4SS_SOURCE_DIR does not look like a RE-UE4SS checkout: ${UE4SS_SOURCE_DIR}")
endif()

# UE4SS public headers include each other across UE4SS/include, the checked-in
# generated member-layout headers, and several deps/first/*/include roots, so
# add every one that exists rather than tracking them by hand.
file(GLOB UE4SS_INCLUDE_DIRS "${UE4SS_SOURCE_DIR}/UE4SS/include")
file(GLOB UE4SS_GENERATED_INCLUDE_DIRS
    "${UE4SS_SOURCE_DIR}/UE4SS/generated_include"
    "${UE4SS_SOURCE_DIR}/deps/first/*/generated_include")
file(GLOB UE4SS_DEP_INCLUDE_DIRS "${UE4SS_SOURCE_DIR}/deps/first/*/include")
# Packages such as Unreal also expose their own namespace directory and a Core
# subdirectory as include roots, because their headers include each other as
# <GenericPlatform/GenericPlatform.hpp> rather than by full path.
file(GLOB UE4SS_NESTED_INCLUDE_DIRS
    "${UE4SS_SOURCE_DIR}/deps/first/*/include/*"
    "${UE4SS_SOURCE_DIR}/deps/first/*/include/*/Core")
foreach(ue4ss_include_dir IN LISTS UE4SS_GENERATED_INCLUDE_DIRS UE4SS_DEP_INCLUDE_DIRS UE4SS_NESTED_INCLUDE_DIRS)
    if(IS_DIRECTORY "${ue4ss_include_dir}")
        list(APPEND UE4SS_INCLUDE_DIRS "${ue4ss_include_dir}")
    endif()
endforeach()

# UE4SS's own headers pull in fmt (DynamicOutput), ImGui plus TextEditor (the
# mod base class exposes a GUI tab) and Zydis (the Unreal headers), none of
# which ship in the RE-UE4SS repository. Pass their include roots here.
if(DEFINED UE4SS_EXTRA_INCLUDE_DIRS)
    foreach(ue4ss_extra_include_dir IN LISTS UE4SS_EXTRA_INCLUDE_DIRS)
        if(IS_DIRECTORY "${ue4ss_extra_include_dir}")
            list(APPEND UE4SS_INCLUDE_DIRS "${ue4ss_extra_include_dir}")
        endif()
    endforeach()
endif()

target_include_directories(${TARGET} PRIVATE ${UE4SS_INCLUDE_DIRS})
target_link_libraries(${TARGET} PRIVATE ws2_32)

# UE4SS declares its API as __declspec(dllimport) for mods, so the link needs an
# import library. Building all of UE4SS just to obtain one would drag in its Rust
# toolchain, so UE4SS_DEF_FILE can point at a module-definition file listing the
# exports of the UE4SS.dll this mod is deployed against; lib.exe turns that into
# the import library.
if(UE4SS_DEF_FILE)
    if(NOT MSVC)
        message(FATAL_ERROR "UE4SS_DEF_FILE requires MSVC (lib.exe)")
    endif()
    if(NOT EXISTS "${UE4SS_DEF_FILE}")
        message(FATAL_ERROR "UE4SS_DEF_FILE not found: ${UE4SS_DEF_FILE}")
    endif()
    set(UE4SS_IMPORT_LIB "${CMAKE_BINARY_DIR}/ue4ss-import/UE4SS.lib")
    add_custom_command(
        OUTPUT "${UE4SS_IMPORT_LIB}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/ue4ss-import"
        # Do not wrap the values in quotes here: CMake already escapes the
        # argument for the shell, and embedded quotes reach LINK verbatim,
        # which then fails with LNK1104 on a path that looks quoted.
        COMMAND "${CMAKE_AR}" /nologo /machine:x64
                "/def:${UE4SS_DEF_FILE}"
                "/out:${UE4SS_IMPORT_LIB}"
        DEPENDS "${UE4SS_DEF_FILE}"
        VERBATIM)
    add_custom_target(ue4ss_import_library DEPENDS "${UE4SS_IMPORT_LIB}")
    add_dependencies(${TARGET} ue4ss_import_library)
    target_link_libraries(${TARGET} PRIVATE "${UE4SS_IMPORT_LIB}")
endif()

set_target_properties(${TARGET} PROPERTIES
    OUTPUT_NAME "main"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/package/ue4ss/Mods/scum_simple_rcon/dlls"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/package/ue4ss/Mods/scum_simple_rcon/dlls"
)

add_custom_command(TARGET ${TARGET} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/package/ue4ss/Mods/scum_simple_rcon"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/ue4ss/Mods/scum_simple_rcon/config.ini"
        "${CMAKE_BINARY_DIR}/package/ue4ss/Mods/scum_simple_rcon/config.ini"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/ue4ss/Mods/scum_simple_rcon/enabled.txt"
        "${CMAKE_BINARY_DIR}/package/ue4ss/Mods/scum_simple_rcon/enabled.txt"
)
