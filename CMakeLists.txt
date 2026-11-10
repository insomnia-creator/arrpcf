cmake_minimum_required(VERSION 3.16)
project(arrpcf LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Vendored JSON parser (MIT, single-file C).
add_library(yyjson STATIC vendor/yyjson.c)
target_include_directories(yyjson PUBLIC vendor)
target_compile_options(yyjson PRIVATE -Os)

add_executable(arrpcf
  src/main.cpp
  src/config.cpp
  src/log.cpp
  src/json.cpp
  src/reactor.cpp
  src/net.cpp
  src/ws.cpp
  src/ipc.cpp
  src/rpc.cpp
  src/bridge.cpp
  src/detect.cpp
  src/process_scan.cpp
  src/util.cpp
)

target_include_directories(arrpcf PRIVATE vendor src)
target_compile_options(arrpcf PRIVATE -Os -Wall -Wextra -Wpedantic)
target_link_libraries(arrpcf PRIVATE yyjson)

if(APPLE)
  find_library(PROC_LIBRARY proc)
  if(PROC_LIBRARY)
    target_link_libraries(arrpcf PRIVATE ${PROC_LIBRARY})
  endif()
  set_target_properties(arrpcf PROPERTIES LINK_FLAGS "-Wl,-dead_strip,-x")
endif()

install(TARGETS arrpcf RUNTIME DESTINATION bin)
