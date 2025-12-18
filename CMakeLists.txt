cmake_minimum_required(VERSION 3.2)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(NOT DEFINED ENV{PSPDEV})
    message(FATAL_ERROR "Please define PSPDEV to point to your SDK path!")
  endif()
endif()

project(pkgi-psp)

find_package(SDL2 REQUIRED)
find_package(CURL REQUIRED)

option(PKGI_ENABLE_DEBUG "enables debug logging over udp multicast" OFF)

if(PKGI_ENABLE_DEBUG)
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -DPKGI_ENABLE_LOGGING=1")
endif()

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -std=gnu11 -G0")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11")

include_directories(
  include
  ${PSPDEV}/psp/include/freetype2
)

link_directories(
  libs
)

# From vita shell CMakeLists - credits TheFlow
FUNCTION(ADD_RESOURCES out_var)
    SET(result)
    FOREACH(in_f ${ARGN})
        SET(out_f "${CMAKE_CURRENT_BINARY_DIR}/${in_f}.o")
        GET_FILENAME_COMPONENT(out_dir ${out_f} DIRECTORY)
        ADD_CUSTOM_COMMAND(OUTPUT ${out_f}
                COMMAND ${CMAKE_COMMAND} -E make_directory ${out_dir}
                COMMAND ${CMAKE_LINKER} -r -b binary -o ${out_f} ${in_f}
                DEPENDS ${in_f}
                WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                COMMENT "Building resource ${out_f}"
                VERBATIM
                )
        LIST(APPEND result ${out_f})
    ENDFOREACH()
    SET(${out_var} "${result}" PARENT_SCOPE)
ENDFUNCTION()

file(GLOB res_files RELATIVE
  ${CMAKE_SOURCE_DIR}
  data/*.png
  data/*.ahx
)
add_resources(apollo_res ${res_files})

add_executable(${PROJECT_NAME}
  ${apollo_res}
  source/ahx.c
  source/pkg2iso.c
  source/depackager.c
  source/loadpng.c
  source/libfont.c
  source/ttf_fonts.c
  source/zip_util.c
  source/pkgi.c
  source/pkgi_aes.c
  source/pkgi_db.c
  source/pkgi_download.c
  source/pkgi_psp.c
  source/pkgi_config.c
  source/pkgi_dialog.c
  source/pkgi_menu.c
)

target_link_libraries(${PROJECT_NAME}
  ${SDL2_LIBRARIES}
  CURL::libcurl
  dbglogger
  freetype
  png
  pspnet
  pspnet_apctl
  mini18n
  zip
  lzma
  bz2
  z
)

create_pbp_file(
  TARGET ${PROJECT_NAME}
  ICON_PATH ${CMAKE_SOURCE_DIR}/ICON0.PNG
  BACKGROUND_PATH NULL
  PREVIEW_PATH NULL
  TITLE "PKGi PSP"
  VERSION 01.10
)

add_custom_target(createzip
  COMMAND @mkdir -p PKGI/LANG
  COMMAND @cp EBOOT.PBP PKGI/
  COMMAND @cp ${CMAKE_SOURCE_DIR}/data/fonts/NotoSansJP-Medium.otf PKGI/FONT.OTF
  COMMAND @cp ${CMAKE_SOURCE_DIR}/data/LANG/* PKGI/LANG/
  COMMAND @rm -fr pkgi-psp.zip
  COMMAND @zip -r pkgi-psp.zip PKGI/
)

add_custom_target(copy
  COMMAND @echo "Copying to $$PSPMS/PSP/GAME/PKGI/EBOOT.PBP ..."
  COMMAND @mkdir -p $$PSPMS/PSP/GAME/PKGI
  COMMAND @cp -v EBOOT.PBP $$PSPMS/PSP/GAME/PKGI/EBOOT.PBP
  DEPENDS ${PROJECT_NAME}
)

add_custom_target(send
  COMMAND @echo "Uploading to ftp://$$PSPIP:1337/ms0:/PSP/GAME/PKGI/EBOOT.PBP ..."
  COMMAND @curl -T EBOOT.PBP ftp://$$PSPIP:1337/ms0:/PSP/GAME/PKGI/EBOOT.PBP
  DEPENDS ${PROJECT_NAME}
)
