cmake_minimum_required(VERSION 3.10)

project(vicos_time32_shim C)

if (VICOS)
  add_library(vicos_time32_shim SHARED
    vicos_time32_shim.c
  )

  target_compile_options(vicos_time32_shim PRIVATE
    -std=c11
    -Wall
    -Wextra
    -Werror
    -U_TIME_BITS
    -D_TIME_BITS=32
    -D_FILE_OFFSET_BITS=64
  )

  target_link_libraries(vicos_time32_shim PRIVATE pthread)
  anki_build_target_license(vicos_time32_shim "ANKI")
endif()
