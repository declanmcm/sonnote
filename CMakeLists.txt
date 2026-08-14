cmake_minimum_required(VERSION 3.20)
project(Sonnote LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(FindPkgConfig)

find_package(Vulkan REQUIRED)
find_package(SDL2 REQUIRED)
find_package(Freetype REQUIRED)
pkg_check_modules(JACK REQUIRED jack)
pkg_check_modules(SNDFILE REQUIRED sndfile)

# ---------- ImGui sources ----------
#file(GLOB IMGUI_SRC
#  extern/imgui/imgui.cpp
#  extern/imgui/imgui_draw.cpp
#  extern/imgui/imgui_tables.cpp
#  extern/imgui/imgui_widgets.cpp
#  extern/imgui/imgui_demo.cpp
#  extern/imgui/backends/imgui_impl_sdl2.cpp
#  extern/imgui/backends/imgui_impl_opengl3.cpp
#)

#add_library(imgui STATIC ${IMGUI_SRC})
#target_include_directories(imgui PUBLIC
#  extern/imgui
#  extern/imgui/backends
#)

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
#imgui
target_link_libraries(SDL2::SDL2)

# ---------- OpenGL ----------
find_package(OpenGL REQUIRED)

# ---------- App ----------
add_executable(app
  src/main.cpp
  src/audio_engine.cpp
  src/vulkan_renderer.cpp
  src/text_renderer.cpp
  src/parser.cpp
  src/utils.cpp
)

target_include_directories(app PRIVATE
  ${JACK_INCLUDE_DIRS}
  ${SNDFILE_INCLUDE_DIRS}
#  extern/msdfgen/core
#  extern/msdfgen/ext
#  extern/msdfgen
#  extern/imgui
#  extern/miniaudio
  ${SDL2_INCLUDE_DIRS}
  ${FREETYPE_INCLUDE_DIRS}
)

target_link_libraries(app PRIVATE
#  imgui
  pthread
#  msdfgen-core
#  msdfgen-ext
  SDL2::SDL2
  OpenGL::GL
  Vulkan::Vulkan
  Freetype::Freetype
  ${JACK_LIBRARIES}
  ${SNDFILE_LIBRARIES}
)

# Some Linux toolchains need these explicitly
if (UNIX AND NOT APPLE)
  target_link_libraries(app PRIVATE dl pthread fftw3f sndfile)
endif()

# Optional: stricter warnings for your code (not third-party)
if (CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(app PRIVATE -Wall -Wextra -Wpedantic -Wno-missing-field-initializers -g)
endif()

