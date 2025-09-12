cmake_minimum_required(VERSION 3.15)
project(dlmalloc LANGUAGES C)

# Generate position-independent code; required for shared libraries on many platforms
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include_directories(
  ${PROJECT_SOURCE_DIR}
)

if(APPLE)
  add_compile_options(-DNDEBUG -ftls-model=initial-exec)
else()
  add_definitions(-DNDEBUG)
endif()

set(CMAKE_BUILD_TYPE RelWithDebInfo)
set(DLMALLOC_SOURCES
  ${PROJECT_SOURCE_DIR}/malloc.c
)

add_compile_options(-DUSE_LOCKS=1)
if(APPLE)
  add_compile_options(-DDARWIN=1)
endif()

add_library(dlmalloc SHARED ${DLMALLOC_SOURCES})
target_compile_definitions(dlmalloc
    PRIVATE
    _REENTRANT=1
)
set_target_properties(dlmalloc PROPERTIES
  OUTPUT_NAME "dlmalloc"
)

