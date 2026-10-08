cmake_minimum_required(VERSION 3.20)
project(KenshiCoop VERSION 0.1.0 LANGUAGES C CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "KenshiCoop must be built for x64 (Kenshi is 64-bit)")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
# Static CRT: the plugin must not depend on a VC++ redistributable being installed.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

option(KC_ASAN "Build tests with AddressSanitizer" OFF)

add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS _WINSOCK_DEPRECATED_NO_WARNINGS)

# ---- third party ----
add_library(enet STATIC
    third_party/enet/callbacks.c third_party/enet/compress.c third_party/enet/host.c
    third_party/enet/list.c third_party/enet/packet.c third_party/enet/peer.c
    third_party/enet/protocol.c third_party/enet/win32.c)
target_include_directories(enet PUBLIC third_party/enet/include)
target_link_libraries(enet PUBLIC ws2_32 winmm)
target_compile_options(enet PRIVATE /W0)

add_library(minhook STATIC
    third_party/minhook/src/buffer.c third_party/minhook/src/hook.c third_party/minhook/src/trampoline.c
    third_party/minhook/src/hde/hde64.c)
target_include_directories(minhook PUBLIC third_party/minhook/include)
target_compile_options(minhook PRIVATE /W0)

# ---- common (game-agnostic multiplayer core) ----
add_library(kc_common STATIC common/src/protocol.cpp common/src/net.cpp common/src/session.cpp)
target_include_directories(kc_common PUBLIC common/include)
target_link_libraries(kc_common PUBLIC enet)
target_compile_options(kc_common PRIVATE /W4 /WX /permissive-)

# ---- tests ----
add_executable(kc_tests tests/test_main.cpp)
target_link_libraries(kc_tests PRIVATE kc_common)
target_compile_options(kc_tests PRIVATE /W4 /permissive-)
if(KC_ASAN)
    target_compile_options(kc_tests PRIVATE /fsanitize=address /Zi)
    target_compile_options(kc_common PRIVATE /fsanitize=address /Zi)
    target_compile_options(enet PRIVATE /fsanitize=address /Zi)
endif()

# ---- plugin ----
if(EXISTS ${CMAKE_SOURCE_DIR}/plugin/CMakeLists.txt)
    add_subdirectory(plugin)
endif()
