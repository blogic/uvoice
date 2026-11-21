# SPDX-License-Identifier: LGPL-2.1-only
#
# uvtransport - ICE and DTLS-SRTP media transport for one stream
# Copyright (C) 2026 John Crispin <john@phrozen.org>
cmake_minimum_required(VERSION 3.13)
project(uvtransport C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_options(-Os -Wall -Wextra -Werror -Wmissing-declarations
		    -Wno-unused-parameter -D_GNU_SOURCE)

option(UVTRANSPORT_TESTS "build the unit suite" OFF)

if(NOT UVTRANSPORT_VERSION)
	file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/../Makefile" _uvt_pkgver
	     REGEX "^PKG_VERSION:=")
	string(REGEX REPLACE "^PKG_VERSION:=" "" UVTRANSPORT_VERSION "${_uvt_pkgver}")
	string(STRIP "${UVTRANSPORT_VERSION}" UVTRANSPORT_VERSION)
	set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
		     "${CMAKE_CURRENT_SOURCE_DIR}/../Makefile")
endif()
if(NOT UVTRANSPORT_VERSION)
	message(FATAL_ERROR "UVTRANSPORT_VERSION not set and PKG_VERSION not found in ../Makefile")
endif()

find_package(PkgConfig REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(LibJuice REQUIRED)
pkg_check_modules(UVSRTP REQUIRED IMPORTED_TARGET uvsrtp)
find_package(Threads REQUIRED)

add_library(uvtransport SHARED uvtransport.c)
target_include_directories(uvtransport PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(uvtransport PUBLIC LibJuice::LibJuice PkgConfig::UVSRTP
		      OpenSSL::SSL OpenSSL::Crypto Threads::Threads)
set_target_properties(uvtransport PROPERTIES VERSION ${UVTRANSPORT_VERSION}
		      SOVERSION ${UVTRANSPORT_VERSION})

install(TARGETS uvtransport LIBRARY DESTINATION lib)
install(FILES include/uvtransport/uvtransport.h DESTINATION include/uvtransport)

configure_file(uvtransport.pc.in ${CMAKE_CURRENT_BINARY_DIR}/uvtransport.pc @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/uvtransport.pc DESTINATION lib/pkgconfig)

if(UVTRANSPORT_TESTS)
	enable_testing()
	add_executable(test_uvtransport test/test_uvtransport.c)
	target_include_directories(test_uvtransport PRIVATE
				   ${CMAKE_CURRENT_SOURCE_DIR}/include)
	target_link_libraries(test_uvtransport uvtransport Threads::Threads)
	add_test(NAME test_uvtransport COMMAND test_uvtransport)
endif()
