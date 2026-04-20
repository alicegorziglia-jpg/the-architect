cmake_minimum_required(VERSION 3.25)
project(PenStreamServer VERSION 1.0.0 LANGUAGES CXX)

# C++20 standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Usar runtime estático de MSVC para evitar dependencias de DLL
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# Find required packages
find_package(spdlog CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

# Windows specific
if(WIN32)
    find_package(DirectXMath REQUIRED)
endif()

# Executable
add_executable(penstream_server
    src/main.cpp
    src/capture/dxgi_capturer.cpp
    src/capture/frame_pool.cpp
    src/encode/nvenc_encoder.cpp
    src/encode/amf_encoder.cpp
    src/network/webrtc_server.cpp
    src/network/udp_transport.cpp
    src/network/packet_builder.cpp
    src/input/input_handler.cpp
    src/input/virtual_input.cpp
    src/config/config_loader.cpp
)

# Subsistema Windows (GUI con WinMain) pero con consola para debug
set_target_properties(penstream_server PROPERTIES
    WIN32_EXECUTABLE TRUE
)

# Agregar shell32 para Shell_NotifyIcon
target_link_libraries(penstream_server PRIVATE
    shell32
)

# Include directories
target_include_directories(penstream_server PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/deps
)

# Link libraries
target_link_libraries(penstream_server PRIVATE
    spdlog::spdlog
    nlohmann_json::nlohmann_json
    d3d11
    dxgi
    user32
    ws2_32
)

# NVENC SDK (optional)
if(ENABLE_NVENC AND DEFINED ENV{NVENC_SDK_PATH})
    target_compile_definitions(penstream_server PRIVATE ENABLE_NVENC)
    target_include_directories(penstream_server PRIVATE
        $ENV{NVENC_SDK_PATH}/Interfaces
    )
    target_link_directories(penstream_server PRIVATE
        $ENV{NVENC_SDK_PATH}/Lib/x64
    )
    target_link_libraries(penstream_server PRIVATE
        nvencodeapi
    )
    message(STATUS "NVENC support enabled")
elseif(ENABLE_NVENC)
    message(WARNING "NVENC_SDK_PATH not set - NVENC support disabled")
endif()

# AMF SDK (optional)
if(ENABLE_AMF AND DEFINED ENV{AMF_SDK_PATH})
    target_compile_definitions(penstream_server PRIVATE ENABLE_AMF)
    target_include_directories(penstream_server PRIVATE
        $ENV{AMF_SDK_PATH}/amf/public/include
    )
    message(STATUS "AMF support enabled")
endif()

# Windows specific definitions
target_compile_definitions(penstream_server PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WINSOCK_DEPRECATED_NO_WARNINGS
    UNICODE
    _UNICODE
)

# Compiler warnings
if(MSVC)
    target_compile_options(penstream_server PRIVATE
        /W4
        /WX-
        /permissive-
    )
else()
    target_compile_options(penstream_server PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )
endif()

# Install
install(TARGETS penstream_server
    RUNTIME DESTINATION bin
)

# Copy config file
install(FILES ../config.json DESTINATION bin)