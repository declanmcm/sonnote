cmake_minimum_required(VERSION 3.20)
project(ComposerPlayground LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ---------- ImGui sources ----------
file(GLOB IMGUI_SRC
  extern/imgui/imgui.cpp
  extern/imgui/imgui_draw.cpp
  extern/imgui/imgui_tables.cpp
  extern/imgui/imgui_widgets.cpp
  extern/imgui/imgui_demo.cpp
  extern/imgui/backends/imgui_impl_sdl2.cpp
  extern/imgui/backends/imgui_impl_opengl3.cpp
)

add_library(imgui STATIC ${IMGUI_SRC})
target_include_directories(imgui PUBLIC
  extern/imgui
  extern/imgui/backends
)

#find_package(Vulkan REQUIRED)
#find_package(SDL2 REQUIRED)

#add_executable(vulkan_demo main.cpp)
#target_link_libraries(vulkan_demo PRIVATE Vulkan::Vulkan SDL2::SDL2)

# ---------- SDL2 (config or pkg-config fallback) ----------
# Try the official CMake package first (provides SDL2::SDL2)
find_package(SDL2 QUIET CONFIG)


if (NOT TARGET SDL2::SDL2)
  # Fallback to CMake's FindSDL2 (if present)
  find_package(SDL2 QUIET)
endif()

if (NOT TARGET SDL2::SDL2)
  # Final fallback: pkg-config
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(SDL2 REQUIRED sdl2)
  add_library(SDL2::SDL2 INTERFACE IMPORTED)
  set_target_properties(SDL2::SDL2 PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${SDL2_INCLUDE_DIRS}"
    INTERFACE_LINK_LIBRARIES "${SDL2_LINK_LIBRARIES}"
    INTERFACE_COMPILE_OPTIONS "${SDL2_CFLAGS_OTHER}"
  )
endif()

# Make sure ImGui can see <SDL.h> by inheriting SDL2's include dirs
target_link_libraries(imgui PUBLIC SDL2::SDL2)

# ---------- OpenGL ----------
find_package(OpenGL REQUIRED)

# ---------- App ----------
add_executable(app
  src/main.cpp
  src/audio_engine.cpp
)

target_include_directories(app PRIVATE
  extern/imgui
  extern/miniaudio
)

target_link_libraries(app PRIVATE
  imgui
  SDL2::SDL2
  OpenGL::GL
)

# Some Linux toolchains need these explicitly
if (UNIX AND NOT APPLE)
  target_link_libraries(app PRIVATE dl pthread)
endif()

# Optional: stricter warnings for your code (not third-party)
if (CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(app PRIVATE -Wall -Wextra -Wpedantic)
endif()

