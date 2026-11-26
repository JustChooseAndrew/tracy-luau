cmake_minimum_required(VERSION 3.20)

project(tracy_roblox_bridge VERSION 0.5.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
  # Static CRT: one .exe with nothing to install next to it.
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

find_package(Threads REQUIRED)

# ---------------------------------------------------------------- Tracy client
# Vendored, unmodified Tracy client (third_party/tracy/VERSION.txt). The viewer must be
# the same Tracy version, or it refuses the connection.
add_library(TracyClient STATIC third_party/tracy/public/TracyClient.cpp)
target_include_directories(TracyClient SYSTEM PUBLIC third_party/tracy/public)
target_compile_definitions(TracyClient PUBLIC
  TRACY_ENABLE
  # Accept viewer connections any time, and as many times as you like.
  TRACY_ON_DEMAND
  # The bridge starts and stops Tracy itself: --convert needs to pick the port first,
  # and stopping cleanly is what hands the last events to the viewer.
  TRACY_DELAYED_INIT
  TRACY_MANUAL_LIFETIME
  # Nanosecond steady clock instead of raw TSC ticks, so Luau's os.clock() seconds
  # map onto Tracy's timeline with a single offset.
  TRACY_DISALLOW_HW_TIMER
  # Never expose the profiler port beyond this machine.
  TRACY_ONLY_LOCALHOST
  # The bridge's own threads aren't interesting; keep the capture to Roblox data.
  TRACY_NO_SAMPLING
  TRACY_NO_SYSTEM_TRACING
  TRACY_NO_CONTEXT_SWITCH
  TRACY_NO_CALLSTACK
  TRACY_NO_CODE_TRANSFER
  TRACY_NO_FRAME_IMAGE
  TRACY_NO_VSYNC_CAPTURE
)
target_link_libraries(TracyClient PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
if(WIN32)
  target_link_libraries(TracyClient PUBLIC ws2_32 dbghelp)
endif()
if(MSVC)
  target_compile_options(TracyClient PRIVATE /utf-8 /W0)
endif()

# ---------------------------------------------------------------- bridge
add_executable(tracy-roblox-bridge
  src/main.cpp
  src/Process.cpp
  src/Recording.cpp
  src/Replay.cpp
)
target_include_directories(tracy-roblox-bridge PRIVATE src)
target_include_directories(tracy-roblox-bridge SYSTEM PRIVATE third_party/httplib)
target_compile_definitions(tracy-roblox-bridge PRIVATE BRIDGE_VERSION="${PROJECT_VERSION}")
target_link_libraries(tracy-roblox-bridge PRIVATE TracyClient)
if(WIN32)
  target_compile_definitions(tracy-roblox-bridge PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00
    _CRT_SECURE_NO_WARNINGS)  # fopen/getenv are fine here; the _s variants aren't portable
endif()
if(MSVC)
  target_compile_options(tracy-roblox-bridge PRIVATE /utf-8 /W4 /permissive- /bigobj)
else()
  target_compile_options(tracy-roblox-bridge PRIVATE -Wall -Wextra)
endif()
