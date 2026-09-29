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
)

if(MSVC)
    target_compile_options(${TARGET} PRIVATE /permissive- /EHsc /bigobj)
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

# UE4SS public headers include each other across UE4SS/include and several
# deps/first/*/include roots, so add every one that exists.
file(GLOB UE4SS_INCLUDE_DIRS "${UE4SS_SOURCE_DIR}/UE4SS/include")
file(GLOB UE4SS_DEP_INCLUDE_DIRS "${UE4SS_SOURCE_DIR}/deps/first/*/include")
foreach(ue4ss_include_dir IN LISTS UE4SS_DEP_INCLUDE_DIRS)
    if(IS_DIRECTORY "${ue4ss_include_dir}")
        list(APPEND UE4SS_INCLUDE_DIRS "${ue4ss_include_dir}")
    endif()
endforeach()

target_include_directories(${TARGET} PRIVATE ${UE4SS_INCLUDE_DIRS})
target_link_libraries(${TARGET} PRIVATE ws2_32)

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
