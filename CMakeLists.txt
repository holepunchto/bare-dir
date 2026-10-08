cmake_minimum_required(VERSION 4.0)

find_package(cmake-bare REQUIRED PATHS node_modules/cmake-bare)
find_package(cmake-fetch REQUIRED PATHS node_modules/cmake-fetch)

project(bare_dir C)

fetch_package("github:holepunchto/libutf#a1ceca8")

add_bare_module(bare_dir)

target_sources(
  ${bare_dir}
  PRIVATE
    binding.c
    lib/dir.h
    lib/name.c
)

target_link_libraries(
  ${bare_dir}
  PRIVATE
    $<TARGET_OBJECTS:utf>
  PUBLIC
    utf
)

if(WIN32)
  target_sources(
    ${bare_dir}
    PRIVATE
      lib/win32.c
  )

  target_link_libraries(
    ${bare_dir}
    PRIVATE
      ntdll
  )
else()
  target_sources(
    ${bare_dir}
    PRIVATE
      lib/posix.c
  )
endif()
