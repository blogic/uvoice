# SPDX-License-Identifier: LGPL-2.1-only
#
# uvsrtp - SRTP/SRTCP per-stream protection over libsrtp2
# Copyright (C) 2026 Adtran Inc.
# Author: Chad Monroe <chad.monroe@adtran.com>
cmake_minimum_required(VERSION 3.13)
project(uvsrtp C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_options(-Os -Wall -Wextra -Werror -Wmissing-declarations
		    -Wno-unused-parameter -D_GNU_SOURCE)

option(UVSRTP_TESTS "build the unit suite" OFF)

# The SONAME version is the package Makefile's PKG_VERSION. The OpenWrt build
# passes -DUVSRTP_VERSION; a bare host build reads it from the Makefile beside
# src/.
if(NOT UVSRTP_VERSION)
	file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/../Makefile" _uvsrtp_pkgver
	     REGEX "^PKG_VERSION:=")
	string(REGEX REPLACE "^PKG_VERSION:=" "" UVSRTP_VERSION "${_uvsrtp_pkgver}")
	string(STRIP "${UVSRTP_VERSION}" UVSRTP_VERSION)
	set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
		     "${CMAKE_CURRENT_SOURCE_DIR}/../Makefile")
endif()
if(NOT UVSRTP_VERSION)
	message(FATAL_ERROR "UVSRTP_VERSION not set and PKG_VERSION not found in ../Makefile")
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(SRTP2 REQUIRED IMPORTED_TARGET libsrtp2)

add_library(uvsrtp SHARED uvsrtp.c)
target_include_directories(uvsrtp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(uvsrtp PUBLIC PkgConfig::SRTP2)
# The ABI changes with the package release, so the SONAME carries the full
# package version, as libudsp does.
set_target_properties(uvsrtp PROPERTIES VERSION ${UVSRTP_VERSION}
		      SOVERSION ${UVSRTP_VERSION})

install(TARGETS uvsrtp LIBRARY DESTINATION lib)
install(FILES include/uvsrtp/uvsrtp.h DESTINATION include/uvsrtp)

configure_file(uvsrtp.pc.in ${CMAKE_CURRENT_BINARY_DIR}/uvsrtp.pc @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/uvsrtp.pc DESTINATION lib/pkgconfig)

if(UVSRTP_TESTS)
	enable_testing()
	add_executable(test_uvsrtp test/test_uvsrtp.c)
	target_include_directories(test_uvsrtp PRIVATE
				   ${CMAKE_CURRENT_SOURCE_DIR}/include)
	target_link_libraries(test_uvsrtp uvsrtp PkgConfig::SRTP2)
	add_test(NAME test_uvsrtp COMMAND test_uvsrtp)
endif()
