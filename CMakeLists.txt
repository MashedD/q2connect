cmake_minimum_required(VERSION 3.20)

project(q2connect VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if (NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_WARN_DEPRECATED OFF CACHE BOOL "" FORCE)

include(FetchContent)

FetchContent_Declare(
  raylib
  GIT_REPOSITORY https://github.com/raysan5/raylib.git
  GIT_TAG 5.5
  GIT_SHALLOW TRUE
  GIT_PROGRESS TRUE
)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_GAMES OFF CACHE BOOL "" FORCE)
set(CUSTOMIZE_BUILD ON CACHE BOOL "" FORCE)

FetchContent_Declare(
  nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.11.3
  GIT_SHALLOW TRUE
  GIT_PROGRESS TRUE
)

FetchContent_MakeAvailable(raylib nlohmann_json)

if (TARGET raylib AND NOT MSVC)
  target_compile_options(raylib PRIVATE -w)
endif()

add_executable(q2connect
  src/main.cpp
)

target_link_libraries(q2connect PRIVATE raylib nlohmann_json::nlohmann_json)

add_custom_command(TARGET q2connect POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          ${CMAKE_CURRENT_SOURCE_DIR}/assets
          $<TARGET_FILE_DIR:q2connect>/assets
)

if (WIN32)
  target_compile_definitions(q2connect PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

if (MSVC)
  target_compile_options(q2connect PRIVATE /W4)
else()
  target_compile_options(q2connect PRIVATE -Wall -Wextra -Wpedantic)
endif()
