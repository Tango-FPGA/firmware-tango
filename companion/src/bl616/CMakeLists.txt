# Modified for Tango, 2026: see the Tango comments; Apache-2.0 as the original.
cmake_minimum_required(VERSION 3.15)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE BOOL "Export compile commands for tooling" FORCE)

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT DEFINED TANG_BOARD OR "${TANG_BOARD}" STREQUAL "")
    set(TANG_BOARD "m0sdock")
endif()

if(TANG_BOARD STREQUAL "mega60k")
    add_definitions(-DTANG_MEGA60K)
elseif(TANG_BOARD STREQUAL "mega138kpro")
    add_definitions(-DTANG_MEGA138KPRO)
elseif(TANG_BOARD STREQUAL "console60k") 
    add_definitions(-DTANG_CONSOLE60K)
elseif(TANG_BOARD STREQUAL "console138k")
    add_definitions(-DTANG_CONSOLE138K)
elseif(TANG_BOARD STREQUAL "primer25k")
    add_definitions(-DTANG_PRIMER25K)
elseif(TANG_BOARD STREQUAL "nano20k")
    add_definitions(-DTANG_NANO20K)
elseif(TANG_BOARD STREQUAL "nano20k_v3923")
    add_definitions(-DTANG_NANO20K_V3923)
elseif(TANG_BOARD STREQUAL "m0sdock")
    add_definitions(-DM0S_DOCK)
else()
   message(FATAL_ERROR "TANG_BOARD must be mega60k, mega138kpro, console60k, primer25k, nano20k, nano20k_v3923 or m0sdock")
endif()

find_package(bouffalo_sdk REQUIRED HINTS $ENV{BL_SDK_BASE})

# Release images carry no build-machine paths. __FILE__ (asserts, e.g. miniz's) expands to the path the compiler was
# given, and CMake gives absolute ones; the SDK maps only its own root (-fmacro-prefix-map=$BL_SDK_BASE=., its
# CMakeLists.txt), so our scratch tree's sources and anything else outside the SDK kept the full path. GCC (10.2 here)
# lets a -ffile-prefix-map (macro + debug maps) take priority over a -fmacro-prefix-map and, among the
# -ffile-prefix-maps, the LAST matching one wins, so these go broadest first: the home directory and the directory
# holding the SDK as fallbacks, the SDK root restated as the SDK's own "." (its ./components/... strings stay
# byte-identical), and the build tree (this file sits at <tree>/src/bl616) last. On sdk_intf_lib, so every compile
# of the build, SDK libraries included, gets them. Paths come from the environment and CMake, never spelled out.
get_filename_component(TANGO_TREE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
get_filename_component(TANGO_SDK_BASE "$ENV{BL_SDK_BASE}" ABSOLUTE)
get_filename_component(TANGO_SDK_PARENT "${TANGO_SDK_BASE}/.." ABSOLUTE)
if(DEFINED ENV{HOME} AND NOT "$ENV{HOME}" STREQUAL "" AND NOT "$ENV{HOME}" STREQUAL "/")
    sdk_add_compile_options("-ffile-prefix-map=$ENV{HOME}=home")
endif()
sdk_add_compile_options("-ffile-prefix-map=${TANGO_SDK_PARENT}=bl616-sdk")
sdk_add_compile_options("-ffile-prefix-map=${TANGO_SDK_BASE}=.")
sdk_add_compile_options("-ffile-prefix-map=${TANGO_TREE_ROOT}=tango-build")

sdk_add_include_directories($ENV{BL_SDK_BASE}/components/net/lwip/lwip/src/include)
sdk_add_include_directories(. ../u8g2/csrc)

# Tango: the launcher + OSD backend (tango/ in this repository, copied to src/tango by build.sh).
# TANGO_NO_NET: never start Wi-Fi, FTP, telnet or the AT modem (the product has no networking).
add_definitions(-DTANGO -DTANGO_NO_NET)
sdk_add_include_directories(.. ../tango)

# tango_zip.c (.zip ROM support) pulls in miniz's tinfl (decompressor only,
# ../tango/third_party/miniz/UPSTREAM.md): these disable everything in the vendored miniz.h/_tdef.h/_zip.h
# headers except the tinfl declarations -- the exact defines the host test Makefile uses, confirmed clean
# there under -Wall -Wextra -Werror. MINIZ_NO_ARCHIVE_WRITING_APIS is not also needed: miniz.h defines it
# itself once MINIZ_NO_ARCHIVE_APIS is set.
add_definitions(-DMINIZ_NO_DEFLATE_APIS -DMINIZ_NO_ARCHIVE_APIS -DMINIZ_NO_ZLIB_APIS)

sdk_add_link_options(-Wl,--wrap=bflb_console_write)

# User
sdk_add_compile_definitions(-DCONFIG_CLI_CMD_ENABLE)

target_sources(app PRIVATE
   	mcu_hw.c
    $ENV{BL_SDK_BASE}/components/net/lwip/lwip/src/apps/sntp/sntp.c
    ../ftpd.c
    ../telnetd.c
    ../hidparser.c
   	../hid.c
   	../inifile.c
   	../menu.c
   	../sdc.c
   	../osd_u8g2.c
   	../sysctrl.c
   	../config.c
   	../xml.c
    ../at_wifi.c
    ../puff.c
    ../jtag.c
    hid_keyboard_template.c
    ../gowin.c
    sdc_direct.c
    ../ps2helper.c
    ../tango/tango_fw.c
    ../tango/tango_launcher.c
    ../tango/tango_browser.c
    ../tango/tango_layout.c
    ../tango/tango_proto.c
    ../tango/tango_font_data.c
    ../tango/tango_coremenu.c
    ../tango/confstr.c
    ../tango/tango_padmap.c
    ../tango/tango_menubtn.c
    ../tango/tango_save.c
    ../tango/tango_kbd.c
    ../tango/tango_volume.c
    ../tango/tango_about.c
    ../tango/tango_vproc.c
    ../tango/tango_zip.c
    ../tango/tango_md5.c
    ../tango/tango_mra.c
    ../tango/tango_hidparse.c
    ../tango/tango_paddb.c
    ../tango/tango_paddb_builtin.c
    ../tango/tango_paddec.c
    ../tango/tango_padres.c
    ../tango/tango_padmgr.c
    ../tango/tango_padlearn.c
    ../tango/third_party/miniz/miniz_tinfl.c
)

file(GLOB COMPONENT_SRCS ../u8g2/csrc/*.c  ../u8g2/sys/bitmap/common/*.c)
target_sources(app PRIVATE ${COMPONENT_SRCS})

sdk_set_main_file(../main.c)

project(fpga_companion)
#project(fpga_companion CXX)
