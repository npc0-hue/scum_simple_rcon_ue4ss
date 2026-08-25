cmake_minimum_required(VERSION 3.22)

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

if(NOT TARGET UE4SS)
    message(FATAL_ERROR
        "UE4SS target not found. Build this as a sibling project next to RE-UE4SS, "
        "or add_subdirectory(RE-UE4SS) before add_subdirectory(scum_simple_rcon_ue4ss).")
endif()

target_link_libraries(${TARGET} PUBLIC UE4SS)
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
