# SPDX-License-Identifier: LGPL-2.1-only
#
# udsp - software voice DSP for FXS lines
# Copyright (C) 2026 Adtran Inc.
# Author: Chad Monroe <chad.monroe@adtran.com>
#
# Build registration: the library, the tools, the benches and the suites.
cmake_minimum_required(VERSION 3.13)
project(udsp C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_options(-Os -Wall -Wextra -Werror -Wmissing-declarations
		    -Wno-unused-parameter -D_GNU_SOURCE)

option(UDSP_TESTS "build the unit and fuzz suites" OFF)
option(UDSP_TOOLS "build udsp-cli and udsp-linecap" OFF)
option(UDSP_BENCH "build the standalone benches" OFF)
option(UDSP_FAX_T30_SWEEP "register the T.30 loss sweep of test_fax_t30" OFF)

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

# The banner version is the package Makefile's PKG_VERSION. The OpenWrt build
# passes -DUDSP_VERSION through CMAKE_OPTIONS; a bare host build reads it from
# the Makefile beside src/.
if(NOT UDSP_VERSION)
	file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/../Makefile" _udsp_pkgver
	     REGEX "^PKG_VERSION:=")
	string(REGEX REPLACE "^PKG_VERSION:=" "" UDSP_VERSION "${_udsp_pkgver}")
	string(STRIP "${UDSP_VERSION}" UDSP_VERSION)
	set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
		     "${CMAKE_CURRENT_SOURCE_DIR}/../Makefile")
endif()
if(NOT UDSP_VERSION)
	message(FATAL_ERROR "UDSP_VERSION not set and PKG_VERSION not found in ../Makefile")
endif()
add_compile_definitions(UDSP_VERSION="${UDSP_VERSION}")

# Per-directory source lists.
set(UDSP_CORE_SRC core/engine.c core/rt.c
    core/udsp.c core/chan.c core/cmd.c core/event.c core/thread.c
    core/slic_thread.c core/region_ctl.c core/safe_state.c core/stream.c
    core/fax_plugin.c
    core/net.c)
# The raw ALSA backend talks to the PCM character devices through the kernel
# uapi header alone, so it builds everywhere the header does and joins the
# library without a probe.
include(CheckIncludeFile)
check_include_file(sound/asound.h UDSP_HAVE_ASOUND_H)
if(NOT UDSP_HAVE_ASOUND_H)
	message(FATAL_ERROR "sound/asound.h not found: the alsa-raw tdm backend needs the Linux uapi headers")
endif()
set(UDSP_TDM_SRC tdm/file.c tdm/alsa_raw.c)
set(UDSP_SLIC_SRC slic/fake.c slic/host_linux.c slic/plugin_load.c)
set(UDSP_DSP_SRC dsp/g711.c dsp/g726.c dsp/cn.c dsp/vad.c
    dsp/goertzel.c dsp/dtmf_det.c dsp/dtmf_gen.c dsp/tone_gen.c dsp/gain.c
    dsp/fsk_mod.c dsp/cid.c dsp/fax_det.c
    dsp/lec.c
    dsp/mix.c
    dsp/resamp.c
    dsp/g722.c dsp/plc.c
    dsp/g729.c dsp/g729_tables.c)
set(UDSP_RTP_SRC rtp/rtp.c rtp/rtcp.c rtp/telev.c rtp/stats.c rtp/jb.c
    rtp/srtp.c)

# libsrtp2 is a hard dependency of the library (a silent no-SRTP libudsp is
# forbidden), so the probe is REQUIRED.
find_package(PkgConfig REQUIRED)
pkg_check_modules(SRTP2 REQUIRED IMPORTED_TARGET libsrtp2)

# The Opus codec is optional. ON requires libopus, OFF
# skips it, AUTO probes: the package Makefile passes ON or OFF from
# CONFIG_UDSP_OPUS so a staging-dir leak cannot flip the link. The codec
# joins the LIBRARY target only; the suites that compile UDSP_DSP_SRC
# directly stay Opus-free, so their registry walk is host-independent.
set(UDSP_OPUS "AUTO" CACHE STRING "build the Opus codec (ON/OFF/AUTO)")
if(UDSP_OPUS STREQUAL "AUTO")
	pkg_check_modules(OPUS QUIET IMPORTED_TARGET opus)
elseif(UDSP_OPUS)
	pkg_check_modules(OPUS REQUIRED IMPORTED_TARGET opus)
endif()
set(UDSP_LINE_SRC line/hook.c line/ring.c line/cidseq.c line/region.c)

# The ALSA probe must run before UDSP_SRC is assembled: set() expands
# immediately, so a later append never reaches the target.
find_path(ALSA_INCLUDE_DIR alsa/asoundlib.h)
find_library(ALSA_LIBRARY NAMES asound)
if(ALSA_INCLUDE_DIR AND ALSA_LIBRARY)
	list(APPEND UDSP_TDM_SRC tdm/alsa.c)
endif()

set(UDSP_SRC ${UDSP_CORE_SRC} ${UDSP_TDM_SRC} ${UDSP_SLIC_SRC}
	     ${UDSP_DSP_SRC} ${UDSP_RTP_SRC} ${UDSP_LINE_SRC})

set(UDSP_REGION_DIR "/usr/share/udsp/regions" CACHE STRING "installed region files")
add_library(udsp SHARED ${UDSP_SRC})
target_compile_definitions(udsp PRIVATE UDSP_REGION_DIR="${UDSP_REGION_DIR}")
# The ABI changes with each release, so the SONAME carries the full package
# version and an image holds one matching libudsp and uvoiced pair.
set_target_properties(udsp PROPERTIES VERSION ${UDSP_VERSION}
		      SOVERSION ${UDSP_VERSION})
install(TARGETS udsp LIBRARY DESTINATION lib)
target_include_directories(udsp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(udsp PUBLIC m pthread dl PkgConfig::SRTP2)
if(ALSA_INCLUDE_DIR AND ALSA_LIBRARY)
	target_compile_definitions(udsp PRIVATE HAVE_ALSA)
	target_include_directories(udsp PRIVATE ${ALSA_INCLUDE_DIR})
	target_link_libraries(udsp PUBLIC ${ALSA_LIBRARY})
endif()
if(TARGET PkgConfig::OPUS)
	target_sources(udsp PRIVATE dsp/opus.c)
	target_compile_definitions(udsp PRIVATE HAVE_OPUS)
	target_link_libraries(udsp PUBLIC PkgConfig::OPUS)
endif()

if(UDSP_TESTS)
	enable_testing()

	# Host-only reference leg for the detectors (spandsp generates the
	# cross-check stimuli); the suite passes without it.
	pkg_check_modules(SPANDSP QUIET IMPORTED_TARGET spandsp)

	add_executable(test_srtp ${CMAKE_CURRENT_SOURCE_DIR}/test/test_srtp.c
		       rtp/srtp.c)
	target_include_directories(test_srtp PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/test)
	target_link_libraries(test_srtp PkgConfig::SRTP2)
	add_test(NAME test_srtp COMMAND test_srtp)

	add_executable(test_tdm ${CMAKE_CURRENT_SOURCE_DIR}/test/test_tdm.c tdm/file.c)
	target_include_directories(test_tdm PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/test)
	add_test(NAME test_tdm COMMAND test_tdm)

	add_executable(test_tdm_raw ${CMAKE_CURRENT_SOURCE_DIR}/test/test_tdm_raw.c
		       tdm/alsa_raw.c)
	target_include_directories(test_tdm_raw PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/test
				   ${CMAKE_CURRENT_SOURCE_DIR}/tdm)
	add_test(NAME test_tdm_raw COMMAND test_tdm_raw)

	add_executable(test_slic_fake ${CMAKE_CURRENT_SOURCE_DIR}/test/test_slic_fake.c slic/fake.c)
	target_include_directories(test_slic_fake PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/test)
	add_test(NAME test_slic_fake COMMAND test_slic_fake)

	add_executable(test_dsp ${CMAKE_CURRENT_SOURCE_DIR}/test/test_dsp.c ${UDSP_DSP_SRC})
	target_include_directories(test_dsp PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/test ${CMAKE_CURRENT_SOURCE_DIR}/dsp)
	target_link_libraries(test_dsp m pthread)
	if(TARGET PkgConfig::SPANDSP)
		target_compile_definitions(test_dsp PRIVATE HAVE_SPANDSP)
		target_link_libraries(test_dsp PkgConfig::SPANDSP)
	endif()
	add_test(NAME test_dsp COMMAND test_dsp)

	add_executable(test_rtp ${CMAKE_CURRENT_SOURCE_DIR}/test/test_rtp.c ${UDSP_RTP_SRC})
	target_include_directories(test_rtp PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/test)
	target_link_libraries(test_rtp m PkgConfig::SRTP2)
	add_test(NAME test_rtp COMMAND test_rtp)

	# The deterministic 10000-case smoke of the fuzz target; the libFuzzer
	# entry stays behind UDSP_LIBFUZZER.
	add_executable(fuzz_rtp ${CMAKE_CURRENT_SOURCE_DIR}/test/fuzz_rtp.c ${UDSP_RTP_SRC})
	target_include_directories(fuzz_rtp PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/test)
	target_link_libraries(fuzz_rtp m PkgConfig::SRTP2)
	add_test(NAME fuzz_rtp COMMAND fuzz_rtp)

	add_executable(test_slic_host ${CMAKE_CURRENT_SOURCE_DIR}/test/test_slic_host.c
		       slic/host_linux.c slic/plugin_load.c)
	target_include_directories(test_slic_host PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/test
			           ${CMAKE_CURRENT_SOURCE_DIR}/slic)
	target_link_libraries(test_slic_host dl pthread)
	add_test(NAME test_slic_host COMMAND test_slic_host)

	add_executable(test_engine ${CMAKE_CURRENT_SOURCE_DIR}/test/test_engine.c
		       core/engine.c core/rt.c core/thread.c tdm/file.c
		       tdm/alsa_raw.c)
	target_include_directories(test_engine PRIVATE
				   ${CMAKE_CURRENT_SOURCE_DIR}/test
				   ${CMAKE_CURRENT_SOURCE_DIR}/tdm)
	target_link_libraries(test_engine pthread)
	add_test(NAME test_engine COMMAND test_engine)

	# The suites that run the whole library link its sources from one
	# object library per Opus setting, built against the source region
	# data. Only test_media takes the Opus build; the others stay
	# Opus-free (see UDSP_OPUS above). core/stream.c and core/net.c
	# reference the rtp and srtp layers, so the rtp sources and libsrtp2
	# join the objects. Registered after the ALSA probe because
	# UDSP_TDM_SRC gains tdm/alsa.c there.
	set(UDSP_TEST_REGION_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../data/regions")
	set(UDSP_TEST_OBJS udsp_test_obj)
	set(UDSP_TEST_OBJ_OPUS udsp_test_obj)
	add_library(udsp_test_obj OBJECT ${UDSP_SRC})
	if(TARGET PkgConfig::OPUS)
		add_library(udsp_test_obj_opus OBJECT ${UDSP_SRC} dsp/opus.c)
		target_compile_definitions(udsp_test_obj_opus PRIVATE HAVE_OPUS)
		target_link_libraries(udsp_test_obj_opus PRIVATE PkgConfig::OPUS)
		set(UDSP_TEST_OBJ_OPUS udsp_test_obj_opus)
		list(APPEND UDSP_TEST_OBJS udsp_test_obj_opus)
	endif()
	foreach(obj IN LISTS UDSP_TEST_OBJS)
		target_compile_definitions(${obj} PRIVATE
			UDSP_REGION_DIR="${UDSP_TEST_REGION_DIR}")
		target_link_libraries(${obj} PRIVATE PkgConfig::SRTP2)
		if(ALSA_INCLUDE_DIR AND ALSA_LIBRARY)
			target_compile_definitions(${obj} PRIVATE HAVE_ALSA)
			target_include_directories(${obj} PRIVATE ${ALSA_INCLUDE_DIR})
		endif()
	endforeach()

	add_executable(test_core ${CMAKE_CURRENT_SOURCE_DIR}/test/test_core.c
		       $<TARGET_OBJECTS:udsp_test_obj>)
	target_include_directories(test_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/test
				   ${CMAKE_CURRENT_SOURCE_DIR}/core
				   ${CMAKE_CURRENT_SOURCE_DIR}/dsp
				   ${CMAKE_CURRENT_SOURCE_DIR}/line
				   ${CMAKE_CURRENT_SOURCE_DIR}/tdm
				   ${CMAKE_CURRENT_SOURCE_DIR}/slic)
	target_compile_definitions(test_core PRIVATE
		UDSP_REGION_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../data/regions")
	target_link_libraries(test_core m pthread dl PkgConfig::SRTP2)
	if(ALSA_INCLUDE_DIR AND ALSA_LIBRARY)
		target_compile_definitions(test_core PRIVATE HAVE_ALSA)
		target_include_directories(test_core PRIVATE ${ALSA_INCLUDE_DIR})
		target_link_libraries(test_core ${ALSA_LIBRARY})
	endif()
	add_test(NAME test_core COMMAND test_core)

	add_executable(test_media ${CMAKE_CURRENT_SOURCE_DIR}/test/test_media.c
		       $<TARGET_OBJECTS:${UDSP_TEST_OBJ_OPUS}>)
	target_include_directories(test_media PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/test
				   ${CMAKE_CURRENT_SOURCE_DIR}/core
				   ${CMAKE_CURRENT_SOURCE_DIR}/dsp
				   ${CMAKE_CURRENT_SOURCE_DIR}/line
				   ${CMAKE_CURRENT_SOURCE_DIR}/slic
				   ${CMAKE_CURRENT_SOURCE_DIR}/rtp)
	target_compile_definitions(test_media PRIVATE
		UDSP_REGION_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../data/regions")
	target_link_libraries(test_media m pthread dl PkgConfig::SRTP2)
	if(ALSA_INCLUDE_DIR AND ALSA_LIBRARY)
		target_compile_definitions(test_media PRIVATE HAVE_ALSA)
		target_include_directories(test_media PRIVATE ${ALSA_INCLUDE_DIR})
		target_link_libraries(test_media ${ALSA_LIBRARY})
	endif()
	if(TARGET PkgConfig::OPUS)		# the Opus stream cases
		target_compile_definitions(test_media PRIVATE HAVE_OPUS)
		target_link_libraries(test_media PkgConfig::OPUS)
	endif()
	# The tick allocates nothing; the allocator stubs are in test_media.c.
	target_link_options(test_media PRIVATE -Wl,--wrap=malloc -Wl,--wrap=free
			    -Wl,--wrap=calloc -Wl,--wrap=realloc)
	# The test-only T.38 gateway, a real shared object so the fax loader
	# and the unload order run through dlopen. Never installed; test_media
	# learns its path from the definition below.
	add_library(fax_plugin_fake MODULE
		    ${CMAKE_CURRENT_SOURCE_DIR}/test/fax_plugin_fake.c)
	target_include_directories(fax_plugin_fake PRIVATE
				   ${CMAKE_CURRENT_SOURCE_DIR}/test)
	set_target_properties(fax_plugin_fake PROPERTIES PREFIX "")
	add_dependencies(test_media fax_plugin_fake)
	target_compile_definitions(test_media PRIVATE
		UDSP_FAX_FAKE_SO="$<TARGET_FILE:fax_plugin_fake>")
	add_test(NAME test_media COMMAND test_media)

	# test_cli embeds tools/udsp-cli.c whole (#define main), so it needs the
	# full library and the source region data.
	add_executable(test_cli ${CMAKE_CURRENT_SOURCE_DIR}/test/test_cli.c
		       $<TARGET_OBJECTS:udsp_test_obj>)
	target_include_directories(test_cli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/test
				   ${CMAKE_CURRENT_SOURCE_DIR}/core
				   ${CMAKE_CURRENT_SOURCE_DIR}/dsp
				   ${CMAKE_CURRENT_SOURCE_DIR}/line
				   ${CMAKE_CURRENT_SOURCE_DIR}/slic
				   ${CMAKE_CURRENT_SOURCE_DIR}/rtp)
	target_compile_definitions(test_cli PRIVATE
		UDSP_REGION_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../data/regions")
	target_link_libraries(test_cli m pthread dl PkgConfig::SRTP2)
	if(ALSA_INCLUDE_DIR AND ALSA_LIBRARY)
		target_compile_definitions(test_cli PRIVATE HAVE_ALSA)
		target_include_directories(test_cli PRIVATE ${ALSA_INCLUDE_DIR})
		target_link_libraries(test_cli ${ALSA_LIBRARY})
	endif()
	add_test(NAME test_cli COMMAND test_cli)

	add_executable(test_g722 ${CMAKE_CURRENT_SOURCE_DIR}/test/test_g722.c
		       dsp/g722.c dsp/plc.c dsp/cn.c)
	target_include_directories(test_g722 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/test
				   ${CMAKE_CURRENT_SOURCE_DIR}/dsp)
	target_link_libraries(test_g722 m)
	# spandsp saturates the G.722 QMF output only since 3.1; an older
	# reference wraps at full scale and inverts the bit identity cases.
	if(SPANDSP_VERSION VERSION_GREATER_EQUAL 3.1)
		target_compile_definitions(test_g722 PRIVATE HAVE_G722_ORACLE)
		target_link_libraries(test_g722 PkgConfig::SPANDSP)
		message(STATUS "spandsp ${SPANDSP_VERSION}: G.722 reference on")
	elseif(SPANDSP_VERSION)
		message(STATUS "spandsp ${SPANDSP_VERSION}: G.722 reference off, 3.1 needed")
	else()
		message(STATUS "no spandsp: G.722 reference off")
	endif()
	add_test(NAME test_g722 COMMAND test_g722)

	add_executable(test_g729 ${CMAKE_CURRENT_SOURCE_DIR}/test/test_g729.c
		       dsp/g729.c dsp/g729_tables.c dsp/cn.c)
	target_include_directories(test_g729 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/test
				   ${CMAKE_CURRENT_SOURCE_DIR}/dsp)
	target_link_libraries(test_g729 m pthread)
	add_test(NAME test_g729 COMMAND test_g729)

	# The suite exists only where libopus does: the codec itself is the
	# optional piece, unlike the g722/g729 references, so an Opus-free host
	# runs one suite fewer rather than a hollow pass.
	if(TARGET PkgConfig::OPUS)
		add_executable(test_opus ${CMAKE_CURRENT_SOURCE_DIR}/test/test_opus.c
			       dsp/opus.c)
		target_include_directories(test_opus PRIVATE
					   ${CMAKE_CURRENT_SOURCE_DIR}/test
					   ${CMAKE_CURRENT_SOURCE_DIR}/dsp)
		target_link_libraries(test_opus m PkgConfig::OPUS)
		add_test(NAME test_opus COMMAND test_opus)
	endif()

	add_executable(test_resamp ${CMAKE_CURRENT_SOURCE_DIR}/test/test_resamp.c
		       dsp/resamp.c)
	target_compile_definitions(test_resamp PRIVATE UDSP_RESAMP_TEST)
	target_include_directories(test_resamp PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/test
				   ${CMAKE_CURRENT_SOURCE_DIR}/dsp)
	target_link_libraries(test_resamp m)
	add_test(NAME test_resamp COMMAND test_resamp)

	add_executable(test_jb ${CMAKE_CURRENT_SOURCE_DIR}/test/test_jb.c rtp/jb.c)
	target_include_directories(test_jb PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/test)
	add_test(NAME test_jb COMMAND test_jb)

	add_executable(test_lec ${CMAKE_CURRENT_SOURCE_DIR}/test/test_lec.c
		       dsp/lec.c dsp/cn.c)
	target_include_directories(test_lec PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/test
				   ${CMAKE_CURRENT_SOURCE_DIR}/dsp)
	target_link_libraries(test_lec m)
	add_test(NAME test_lec COMMAND test_lec)

	add_executable(test_cid ${CMAKE_CURRENT_SOURCE_DIR}/test/test_cid.c
		       dsp/fsk_mod.c dsp/cid.c)
	target_include_directories(test_cid PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/test
				   ${CMAKE_CURRENT_SOURCE_DIR}/dsp)
	target_link_libraries(test_cid m)
	add_test(NAME test_cid COMMAND test_cid)

	add_executable(test_fax_det ${CMAKE_CURRENT_SOURCE_DIR}/test/test_fax_det.c
		       dsp/fax_det.c dsp/goertzel.c)
	target_include_directories(test_fax_det PRIVATE
				   ${CMAKE_CURRENT_SOURCE_DIR}/test
				   ${CMAKE_CURRENT_SOURCE_DIR}/dsp)
	target_link_libraries(test_fax_det m)
	if(TARGET PkgConfig::SPANDSP)
		target_compile_definitions(test_fax_det PRIVATE HAVE_SPANDSP)
		target_link_libraries(test_fax_det PkgConfig::SPANDSP)
	endif()
	add_test(NAME test_fax_det COMMAND test_fax_det)

	# A real T.30 page through the G.711 passthrough path, under the loss
	# schedules of captured field calls. The suite exists only where
	# spandsp 3.1 and libtiff do, the test_opus rule. Its T.30 handlers
	# take the spandsp 3.1 signatures, which spandsp 0.0.6 declares with
	# other arguments, and the page source and sink of spandsp work on
	# TIFF files. Both are host test needs only; the library and the
	# package Makefile never see them. The sweep is the same binary behind
	# UDSP_FAX_T30_SWEEP, outside the sanitizer runs.
	pkg_check_modules(TIFF QUIET IMPORTED_TARGET libtiff-4)
	if(SPANDSP_VERSION VERSION_GREATER_EQUAL 3.1 AND TARGET PkgConfig::TIFF)
		add_executable(test_fax_t30 ${CMAKE_CURRENT_SOURCE_DIR}/test/test_fax_t30.c
			       $<TARGET_OBJECTS:udsp_test_obj>)
		target_include_directories(test_fax_t30 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/test
					   ${CMAKE_CURRENT_SOURCE_DIR}/core
					   ${CMAKE_CURRENT_SOURCE_DIR}/dsp
					   ${CMAKE_CURRENT_SOURCE_DIR}/line
					   ${CMAKE_CURRENT_SOURCE_DIR}/slic
					   ${CMAKE_CURRENT_SOURCE_DIR}/rtp)
		target_compile_definitions(test_fax_t30 PRIVATE
			UDSP_REGION_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../data/regions")
		target_link_libraries(test_fax_t30 m pthread dl PkgConfig::SRTP2
				      PkgConfig::SPANDSP PkgConfig::TIFF)
		if(ALSA_INCLUDE_DIR AND ALSA_LIBRARY)
			target_compile_definitions(test_fax_t30 PRIVATE HAVE_ALSA)
			target_include_directories(test_fax_t30 PRIVATE ${ALSA_INCLUDE_DIR})
			target_link_libraries(test_fax_t30 ${ALSA_LIBRARY})
		endif()
		add_test(NAME test_fax_t30 COMMAND test_fax_t30)
		if(UDSP_FAX_T30_SWEEP)
			add_test(NAME test_fax_t30_sweep COMMAND test_fax_t30 --sweep)
		endif()
		message(STATUS "spandsp ${SPANDSP_VERSION}, libtiff ${TIFF_VERSION}: "
			       "test_fax_t30 on, sweep ${UDSP_FAX_T30_SWEEP}")
	elseif(TARGET PkgConfig::TIFF)
		message(STATUS "spandsp ${SPANDSP_VERSION}: test_fax_t30 off, 3.1 needed")
	else()
		message(STATUS "spandsp ${SPANDSP_VERSION}, no libtiff: test_fax_t30 off")
	endif()

	# test_line carries its own stubs of the dsp/cid.h symbols, so it must
	# never link dsp/cid.c or dsp/fsk_mod.c.
	add_executable(test_line ${CMAKE_CURRENT_SOURCE_DIR}/test/test_line.c
		       line/hook.c line/ring.c line/cidseq.c)
	target_include_directories(test_line PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/test
				   ${CMAKE_CURRENT_SOURCE_DIR}/line)
	add_test(NAME test_line COMMAND test_line)

	add_executable(test_region ${CMAKE_CURRENT_SOURCE_DIR}/test/test_region.c
		       line/region.c)
	target_include_directories(test_region PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/test
				   ${CMAKE_CURRENT_SOURCE_DIR}/line)
	add_test(NAME test_region COMMAND test_region
		 ${CMAKE_CURRENT_SOURCE_DIR}/../data/regions)

	# test_linecap drives a private build of tools/udsp-linecap.c as a child
	# on the file back end, against the source region data, so the suite
	# runs with UDSP_TOOLS off and never depends on an installed tool.
	add_executable(udsp-linecap-test ${CMAKE_CURRENT_SOURCE_DIR}/tools/udsp-linecap.c
		       $<TARGET_OBJECTS:udsp_test_obj>)
	target_compile_definitions(udsp-linecap-test PRIVATE
		UDSP_REGION_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../data/regions")
	target_link_libraries(udsp-linecap-test m pthread dl PkgConfig::SRTP2)
	if(ALSA_INCLUDE_DIR AND ALSA_LIBRARY)
		target_compile_definitions(udsp-linecap-test PRIVATE HAVE_ALSA)
		target_include_directories(udsp-linecap-test PRIVATE ${ALSA_INCLUDE_DIR})
		target_link_libraries(udsp-linecap-test ${ALSA_LIBRARY})
	endif()
	add_executable(test_linecap ${CMAKE_CURRENT_SOURCE_DIR}/test/test_linecap.c)
	target_include_directories(test_linecap PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/test)
	add_test(NAME test_linecap COMMAND test_linecap $<TARGET_FILE:udsp-linecap-test>)

	# The host-side readers carry their own self-tests: synthetic captures
	# through the mu-law round trip, decoded back to the bytes they encode.
	find_program(UDSP_PYTHON3 python3)
	if(UDSP_PYTHON3)
		add_test(NAME test_fsk_demod COMMAND ${UDSP_PYTHON3}
			 ${CMAKE_CURRENT_SOURCE_DIR}/tools/fsk-demod.py --self-test)
		add_test(NAME test_linecap_analyse COMMAND ${UDSP_PYTHON3}
			 ${CMAKE_CURRENT_SOURCE_DIR}/tools/linecap-analyse.py --self-test)
		# The canceller capture reader runs lec_replay on the replay files
		# it writes when the bench build has one. The fixture setup writes
		# a replay file with documented values, and test_lec reads it back
		# through the C reader of test/bench/lec_replay.h.
		set(UDSP_LEC_CAPTURE ${CMAKE_CURRENT_SOURCE_DIR}/tools/lec-capture.py)
		set(UDSP_LEC_FIXTURE ${CMAKE_CURRENT_BINARY_DIR}/lec-capture-fixture.lecr)
		if(UDSP_BENCH)
			add_test(NAME test_lec_capture COMMAND ${UDSP_PYTHON3}
				 ${UDSP_LEC_CAPTURE} --self-test
				 --lec-replay $<TARGET_FILE:lec_replay>)
		else()
			add_test(NAME test_lec_capture COMMAND ${UDSP_PYTHON3}
				 ${UDSP_LEC_CAPTURE} --self-test)
		endif()
		# The echo path fit needs numpy. Without it, the self-test prints a
		# skip line and passes, so the entry depends on python3 alone. One
		# BLAS thread keeps the run time steady when ctest runs in parallel.
		add_test(NAME test_path_fit COMMAND ${UDSP_PYTHON3}
			 ${CMAKE_CURRENT_SOURCE_DIR}/test/tools/path_fit.py --self-test)
		set_tests_properties(test_path_fit PROPERTIES ENVIRONMENT
				     "OPENBLAS_NUM_THREADS=1;OMP_NUM_THREADS=1")
		add_test(NAME test_lec_capture_fixture COMMAND ${UDSP_PYTHON3}
			 ${UDSP_LEC_CAPTURE} --write-fixture ${UDSP_LEC_FIXTURE})
		set_tests_properties(test_lec_capture_fixture PROPERTIES
				     FIXTURES_SETUP lec_capture_fixture)
		set_tests_properties(test_lec PROPERTIES
				     FIXTURES_REQUIRED lec_capture_fixture
				     ENVIRONMENT UDSP_LEC_CAPTURE_FIXTURE=${UDSP_LEC_FIXTURE})
	else()
		message(STATUS "python3 not found: the tool self-tests are skipped")
	endif()
endif()

if(UDSP_BENCH)
	add_executable(bench_dtmf
		       ${CMAKE_CURRENT_SOURCE_DIR}/test/bench/bench_dtmf.c
		       dsp/dtmf_det.c dsp/goertzel.c)
	target_include_directories(bench_dtmf PRIVATE
				   ${CMAKE_CURRENT_SOURCE_DIR}/dsp)
	target_link_libraries(bench_dtmf m)

	# The host replay of a canceller capture needs no speexdsp. The cache
	# path selects the canceller source, so the same program can replay a
	# capture against another copy of dsp/lec.c. The dsp directory is on the
	# include path because a copy outside the tree still includes dsp.h and
	# lec.h by their bare names.
	set(UDSP_REPLAY_LEC_C "dsp/lec.c" CACHE FILEPATH
	    "the canceller source that lec_replay links")
	add_executable(lec_replay
		       ${CMAKE_CURRENT_SOURCE_DIR}/test/bench/lec_replay.c
		       ${UDSP_REPLAY_LEC_C} dsp/cn.c)
	target_include_directories(lec_replay PRIVATE
				   ${CMAKE_CURRENT_SOURCE_DIR}/dsp)
	target_link_libraries(lec_replay m)

	pkg_check_modules(SPEEXDSP QUIET speexdsp)
	if(SPEEXDSP_FOUND)
		add_executable(bench_lec
			       ${CMAKE_CURRENT_SOURCE_DIR}/test/bench/bench_lec.c
			       dsp/lec.c dsp/cn.c dsp/resamp.c)
		target_include_directories(bench_lec PRIVATE
					   ${CMAKE_CURRENT_SOURCE_DIR}/dsp
					   ${SPEEXDSP_INCLUDE_DIRS})
		target_link_directories(bench_lec PRIVATE ${SPEEXDSP_LIBRARY_DIRS})
		target_link_libraries(bench_lec m ${SPEEXDSP_LIBRARIES})
	else()
		message(STATUS "speexdsp not found: bench_lec skipped")
	endif()
endif()

if(UDSP_TOOLS)
	add_executable(udsp-cli tools/udsp-cli.c)
	target_link_libraries(udsp-cli udsp m)
	install(TARGETS udsp-cli RUNTIME DESTINATION bin)

	# The line capture bench tool. Its host-side
	# readers, tools/linecap-analyse.py and tools/fsk-demod.py, are not
	# installed on the target; they read the capture on the host.
	add_executable(udsp-linecap tools/udsp-linecap.c)
	target_link_libraries(udsp-linecap udsp pthread)
	install(TARGETS udsp-linecap RUNTIME DESTINATION bin)
endif()
