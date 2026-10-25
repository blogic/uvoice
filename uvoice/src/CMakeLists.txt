# SPDX-License-Identifier: GPL-2.0-only
#
# uvoice - native voice stack for OpenWrt
# Copyright (C) 2026 Adtran Inc.
# Author: Chad Monroe <chad.monroe@adtran.com>
#
# Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
cmake_minimum_required(VERSION 3.13)

project(uvoice C)

set(UVOICE_HAL "stub" CACHE STRING "HAL backend: stub or udsp")
set_property(CACHE UVOICE_HAL PROPERTY STRINGS stub udsp)

option(UVOICE_TESTS "build the unit test suites and the fuzz targets" OFF)

# The signaling protocols the image carries. voip.base.protocol selects the
# one that runs (proto/proto.c), so an image without a protocol could carry no
# call at all.
option(UVOICE_PROTO_SIP "SIP signaling" ON)
option(UVOICE_PROTO_MGCP "MGCP signaling" OFF)
# The guard refuses a configure that selects no protocol unit: the image
# needs at least one of the two options.
if(NOT UVOICE_PROTO_SIP AND NOT UVOICE_PROTO_MGCP)
	message(FATAL_ERROR
		"UVOICE_PROTO_SIP and UVOICE_PROTO_MGCP are both OFF, so the "
		"image would carry no signaling protocol. Turn at least one of "
		"them on.")
endif()

# udsp backend only: where the staged libudsp headers and library live.
# The libudsp package stages udsp/*.h into <staging>/usr/include/udsp and
# the library into <staging>/usr/lib (udsp/Makefile Build/InstallDev), so
# a buildroot needs neither variable.
set(UVOICE_UDSP_INCLUDEDIR "" CACHE PATH "directory holding udsp/udsp.h")
set(UVOICE_UDSP_LIBDIR "" CACHE PATH "directory holding libudsp")
# test_hal_udsp compiles the whole libudsp source tree. The package stages
# only slic/host_linux.c and slic/plugin_load.c, for the probe tool of a SLIC
# plug-in, so the suite needs the tree itself. Empty means "look beside
# uvoice", which is where the feed has it.
set(UVOICE_UDSP_SRCDIR "" CACHE PATH "libudsp source tree (udsp/src)")

add_definitions(-Os -Wall -Wextra -Werror -Wmissing-declarations
		-Wno-unused-parameter -D_GNU_SOURCE)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# The daemon version is the package Makefile's PKG_VERSION. The OpenWrt build
# passes -DUVOICE_VERSION through CMAKE_OPTIONS; a bare host build reads it from
# the Makefile beside src/.
if(NOT UVOICE_VERSION)
	file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/../Makefile" _uvoice_pkgver
	     REGEX "^PKG_VERSION:=")
	string(REGEX REPLACE "^PKG_VERSION:=" "" UVOICE_VERSION "${_uvoice_pkgver}")
	string(STRIP "${UVOICE_VERSION}" UVOICE_VERSION)
	set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
		     "${CMAKE_CURRENT_SOURCE_DIR}/../Makefile")
endif()
if(NOT UVOICE_VERSION)
	message(FATAL_ERROR "UVOICE_VERSION not set and PKG_VERSION not found in ../Makefile")
endif()
add_compile_definitions(UVOICE_VERSION="${UVOICE_VERSION}")

# The version of the ubus API schema. Every call trace manifest names it, so a
# reader of an archive knows which contract its snapshots follow. It is read
# from the schema document itself, because a second copy in C would drift from
# the file it claims to describe. A build that does not carry schema/ passes
# -DUVOICE_SCHEMA_VERSION, the way it passes -DUVOICE_VERSION.
set(UVOICE_SCHEMA_VERSION "" CACHE STRING "version of the ubus API schema")
if(NOT UVOICE_SCHEMA_VERSION)
	set(_uvoice_schema
	    "${CMAKE_CURRENT_SOURCE_DIR}/../schema/uvoice-api-schema.json")
	if(EXISTS "${_uvoice_schema}")
		file(STRINGS "${_uvoice_schema}" _uvoice_schemaver
		     REGEX "^  \"version\": ")
		string(REGEX REPLACE "^.*\"version\"[ ]*:[ ]*\"([0-9]+\\.[0-9]+\\.[0-9]+)\".*$"
		       "\\1" UVOICE_SCHEMA_VERSION "${_uvoice_schemaver}")
		set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
			     "${_uvoice_schema}")
	endif()
endif()
if(NOT UVOICE_SCHEMA_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
	message(FATAL_ERROR
		"UVOICE_SCHEMA_VERSION not set and no dotted triple found in the "
		"version key of ../schema/uvoice-api-schema.json. The call "
		"trace manifest names the schema its snapshots follow, so the "
		"version has to come from the document or from the command line.")
endif()
add_compile_definitions(UVOICE_SCHEMA_VERSION="${UVOICE_SCHEMA_VERSION}")

find_path(UBOX_INCLUDE_DIR libubox/uloop.h)
find_library(UBOX_LIBRARY NAMES ubox)
# The call trace renders its snapshots as JSON text, so the daemon links the
# blobmsg json renderer beside libubox itself; test_ubus asserts JSON with it.
find_library(BLOBMSG_JSON_LIBRARY NAMES blobmsg_json)
find_path(UBUS_INCLUDE_DIR libubus.h)
find_library(UBUS_LIBRARY NAMES ubus)
find_path(UCI_INCLUDE_DIR uci.h)
find_library(UCI_LIBRARY NAMES uci)
# The SIP unit hashes digests and runs TLS through OpenSSL, and nothing else
# links it.
if(UVOICE_PROTO_SIP)
	find_package(OpenSSL REQUIRED)
endif()

foreach(dep UBOX_INCLUDE_DIR UBOX_LIBRARY BLOBMSG_JSON_LIBRARY UBUS_INCLUDE_DIR
	    UBUS_LIBRARY UCI_INCLUDE_DIR UCI_LIBRARY)
	if(NOT ${dep})
		message(FATAL_ERROR
			"${dep} not found. Point cmake at a tree that has libubox, "
			"libubus and libuci, for example "
			"-DCMAKE_PREFIX_PATH=/path/to/prefix.")
	endif()
endforeach()

# The udebug rings of the call trace. A libubox without libubox/udebug.h has
# no producer API, so the ring half of trace_ring.c is compiled against this
# probe and answers -ENOSYS without it; the daemon then still builds, logs and
# refuses an arm.
include(CheckIncludeFile)
set(CMAKE_REQUIRED_INCLUDES ${UBOX_INCLUDE_DIR})
check_include_file(libubox/udebug.h HAVE_UDEBUG_H)
unset(CMAKE_REQUIRED_INCLUDES)
if(HAVE_UDEBUG_H)
	add_definitions(-DUVOICE_HAVE_UDEBUG)
else()
	message(STATUS "libubox/udebug.h not found, the call trace rings are "
		"compiled out")
endif()

# The core network services every protocol reaches through the host table
# (proto/proto.h): the netifd tracker, the media port pool, the conntrack bounds
# and the firewall reconcile. A suite that drives them compiles the list into
# itself.
set(UVOICE_NET_SOURCES net/iface.c net/ports.c net/conntrack.c
    net/fw_reconcile.c)

# The core sources of uvoiced. The HAL arm below appends the backend, and
# uvoice_core takes the whole list. dialplan.c is the one core object a
# protocol links (proto/proto.h, the link rule).
set(SOURCES main.c event_dispatch.c ubus.c fw.c cfg/uci.c line/line.c cdr.c
	    event.c calltrace.c status.c trace.c trace_ring.c pcapng.c
	    spawn_run.c media_trace.c uvoice_random.c dialplan.c proto/proto.c
	    ${UVOICE_NET_SOURCES})

# Every source includes by its path from src/ (proto/shared/msg.h,
# proto/sip/fsm.h). src/proto/sip must stay off the include path: its module
# name features.h would shadow glibc <features.h> for every TU that inherits
# the directory.

# The objects and the link of the protocols the image carries, beside the core
# list, for uvoiced.
set(UVOICE_PROTO_OBJECTS "")
set(UVOICE_PROTO_LIBRARIES "")

# The shared protocol sources, which every protocol that negotiates SDP links:
# the SDP layer with the SIP message and header text helpers it parses with,
# the media fill of a negotiated result, the protocol timer, the transport
# destination and the asynchronous DNS resolver. The media fill logs through
# ulog, and the timer and the resolver carry uloop handles, so the object
# carries a public libubox link that every consumer inherits. The resolver
# traces through the host table in the domain its caller names. The MGCP
# layers write through the text helpers, run on the timer and resolve their
# call agent, so the set exists under either option, and uvoiced links it
# with either unit.
if(UVOICE_PROTO_SIP OR UVOICE_PROTO_MGCP)
	add_library(uvoice_sdp OBJECT proto/shared/sdp.c proto/shared/msg.c
		    proto/shared/hdr.c proto/shared/media_fill.c
		    proto/shared/timer.c proto/shared/dst.c
		    proto/shared/resolve.c)
	target_include_directories(uvoice_sdp PUBLIC
				   ${CMAKE_CURRENT_SOURCE_DIR}
				   ${UBOX_INCLUDE_DIR})
	target_link_libraries(uvoice_sdp PUBLIC ${UBOX_LIBRARY})
endif()

# The SIP protocol unit, src/proto/sip, in five object sets. A suite links the
# sets its code under test reaches, beside uvoice_sdp.
#
# uvoice_sip: the stream framer and digest authentication. The framer frames
# bytes into messages and touches no socket. auth.c hashes through OpenSSL, so
# the object carries a public OpenSSL::Crypto link that every consumer
# inherits.
#
# uvoice_sip_txn: the transaction layer, the UA core, the registration client,
# the dialog layer, the keepalive client and the subscription client. They add
# the libubox avl for the two transaction match indexes and nothing else: no
# socket. The SIP timer helpers of sip/txn.h are aliases of the protocol timer
# of uvoice_sdp.
#
# uvoice_sip_udp: the parts that own a UDP socket or a uloop handle: the UDP
# transport and the uloop timer environment of the transaction layer, with
# the SIP message tap. The TCP and TLS sockets are in uvoice_sip_stream below.
# transport.c writes the sip trace domain, and trace.c is a core object:
# uvoiced lists it with the core sources, and every suite that links this
# object compiles trace.c into itself.
#
# uvoice_sip_stream: the stream transport, TCP/TLS connections, listeners and
# the OpenSSL wrapper. A separate object so the UDP transport and its suite
# stay exactly as they are, and so the TLS link dependency stops here.
#
# uvoice_proto_sip: the ops table, the SIP half of the line shell, the
# account layer, the call legs, the FXS state machine and the star code
# table. Only uvoiced links it: each suite that drives one of them compiles it
# into itself beside fakes of the others.
if(UVOICE_PROTO_SIP)
	add_library(uvoice_sip OBJECT proto/sip/auth.c proto/sip/frame.c)
	target_include_directories(uvoice_sip PUBLIC
				   ${CMAKE_CURRENT_SOURCE_DIR})
	target_link_libraries(uvoice_sip PUBLIC OpenSSL::Crypto)

	add_library(uvoice_sip_txn OBJECT proto/sip/txn.c proto/sip/ua.c
		    proto/sip/reg.c proto/sip/dialog.c proto/sip/keepalive.c
		    proto/sip/sub.c)
	target_include_directories(uvoice_sip_txn PUBLIC
				   ${CMAKE_CURRENT_SOURCE_DIR}
				   ${UBOX_INCLUDE_DIR})

	add_library(uvoice_sip_udp OBJECT proto/sip/txn_uloop.c
		    proto/sip/transport.c proto/sip/tap.c)
	target_include_directories(uvoice_sip_udp PUBLIC
				   ${CMAKE_CURRENT_SOURCE_DIR}
				   ${UBOX_INCLUDE_DIR})

	add_library(uvoice_sip_stream OBJECT proto/sip/stream.c
		    proto/sip/tls.c)
	target_include_directories(uvoice_sip_stream PUBLIC
				   ${CMAKE_CURRENT_SOURCE_DIR}
				   ${UBOX_INCLUDE_DIR})
	target_link_libraries(uvoice_sip_stream PRIVATE OpenSSL::SSL)

	add_library(uvoice_proto_sip OBJECT proto/sip/proto.c
		    proto/sip/line.c proto/sip/account.c proto/sip/call.c
		    proto/sip/fsm.c proto/sip/desk.c proto/sip/features.c)
	target_include_directories(uvoice_proto_sip PRIVATE
				   ${CMAKE_CURRENT_SOURCE_DIR}
				   ${UBOX_INCLUDE_DIR} ${UBUS_INCLUDE_DIR})

	list(APPEND UVOICE_PROTO_OBJECTS uvoice_sdp uvoice_sip uvoice_sip_txn
	     uvoice_sip_udp uvoice_sip_stream uvoice_proto_sip)
	list(APPEND UVOICE_PROTO_LIBRARIES OpenSSL::SSL OpenSSL::Crypto)
endif()

# The MGCP protocol unit, src/proto/mgcp, in four object sets. A suite links
# the sets its code under test reaches, beside uvoice_sdp. Only
# uvoice_proto_mgcp owns a socket.
#
# uvoice_proto_mgcp_text: the message parser and builders. They write
# through the span and buffer helpers of shared/msg.c and hdr.c in uvoice_sdp.
#
# uvoice_proto_mgcp_txn: the transaction layer. It runs on the protocol timer
# of uvoice_sdp and on the text layer.
#
# uvoice_proto_mgcp_ep: the endpoints and the package tables. They reach the
# core through the host table alone, compile and match digit maps through
# dialplan.c, and send through the transaction layer.
#
# uvoice_proto_mgcp: the ops table, the socket, the addresses of the call
# agent through the resolver of uvoice_sdp, and the status dumps. No set
# needs OpenSSL, so an image with MGCP alone links none.
if(UVOICE_PROTO_MGCP)
	add_library(uvoice_proto_mgcp_text OBJECT proto/mgcp/msg.c)
	target_include_directories(uvoice_proto_mgcp_text PUBLIC
				   ${CMAKE_CURRENT_SOURCE_DIR})

	add_library(uvoice_proto_mgcp_txn OBJECT proto/mgcp/txn.c)
	target_include_directories(uvoice_proto_mgcp_txn PUBLIC
				   ${CMAKE_CURRENT_SOURCE_DIR})

	add_library(uvoice_proto_mgcp_ep OBJECT proto/mgcp/sig.c
		    proto/mgcp/ep.c)
	target_include_directories(uvoice_proto_mgcp_ep PUBLIC
				   ${CMAKE_CURRENT_SOURCE_DIR}
				   ${UBOX_INCLUDE_DIR})

	add_library(uvoice_proto_mgcp OBJECT proto/mgcp/proto.c)
	target_include_directories(uvoice_proto_mgcp PRIVATE
				   ${CMAKE_CURRENT_SOURCE_DIR}
				   ${UBOX_INCLUDE_DIR})

	list(APPEND UVOICE_PROTO_OBJECTS uvoice_sdp uvoice_proto_mgcp_text
	     uvoice_proto_mgcp_txn uvoice_proto_mgcp_ep uvoice_proto_mgcp)
endif()
# uvoice_sdp is on the list once when both units are on
list(REMOVE_DUPLICATES UVOICE_PROTO_OBJECTS)

# Backend private include and link additions, empty for the stub build.
set(HAL_INCLUDE_DIRS "")
set(HAL_LIBRARIES "")

if(UVOICE_HAL STREQUAL "stub")
	list(APPEND SOURCES hal/stub/stub.c)
	add_definitions(-DUVOICE_HAL_STUB)
elseif(UVOICE_HAL STREQUAL "udsp")
	add_definitions(-DUVOICE_HAL_UDSP)
	list(APPEND SOURCES hal/udsp/udsp.c hal/udsp/udsp_events.c
	     hal/hal_relay.c hal/hal_media.c)

	# The libudsp package stages the headers and the library into the
	# staging tree, so a buildroot finds both on the default path. The two
	# cache variables let a bare cross or host build name a copy of either.
	# An explicit variable is authoritative: the event ABI asserts of
	# hal/udsp/udsp_events.c are tied to the headers this build names, so
	# another copy out of a staging tree would assert against the wrong
	# contract.
	if(UVOICE_UDSP_INCLUDEDIR)
		find_path(UDSP_INCLUDE_DIR udsp/udsp.h HINTS ${UVOICE_UDSP_INCLUDEDIR}
			  NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
	else()
		find_path(UDSP_INCLUDE_DIR udsp/udsp.h)
	endif()

	if(UVOICE_UDSP_LIBDIR)
		find_library(UDSP_LIBRARY NAMES udsp HINTS ${UVOICE_UDSP_LIBDIR}
			     NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
	else()
		find_library(UDSP_LIBRARY NAMES udsp)
	endif()

	foreach(dep UDSP_INCLUDE_DIR UDSP_LIBRARY)
		if(NOT ${dep})
			message(FATAL_ERROR
				"${dep} not found. The udsp backend needs the "
				"libudsp headers and libudsp. Pass "
				"-DUVOICE_UDSP_INCLUDEDIR=<dir with udsp/udsp.h> "
				"and -DUVOICE_UDSP_LIBDIR=<dir with libudsp>.")
		endif()
	endforeach()

	list(APPEND HAL_INCLUDE_DIRS ${UDSP_INCLUDE_DIR})
	# libudsp is a shared library. Its NEEDED entries name the libraries
	# it calls (libsrtp2, libasound, and libopus in an Opus build), and
	# uvoiced inherits them, so uvoiced names none of them.
	list(APPEND HAL_LIBRARIES ${UDSP_LIBRARY})
elseif(UVOICE_HAL STREQUAL "css")
	add_definitions(-DUVOICE_HAL_CSS)
	list(APPEND SOURCES hal/css/css.c hal/css/css_media.c hal/css/css_line.c
	     hal/css/css_input.c hal/css/css_audio.c hal/css/css_tone.c
	     hal/hal_relay.c hal/hal_media.c)

	# The css backend synthesises its tones and ringer and drives the
	# analogue mixer through alsa-lib, and protects the media with libuvsrtp.
	find_package(PkgConfig REQUIRED)
	pkg_check_modules(ALSA REQUIRED IMPORTED_TARGET alsa)
	list(APPEND HAL_LIBRARIES PkgConfig::ALSA pthread)

	# libuvsrtp, from the staging tree in a buildroot or a named copy. The
	# two cache variables let a bare cross or host build find it.
	if(UVOICE_UVSRTP_INCLUDEDIR)
		find_path(UVSRTP_INCLUDE_DIR uvsrtp/uvsrtp.h
			  HINTS ${UVOICE_UVSRTP_INCLUDEDIR}
			  NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
	else()
		find_path(UVSRTP_INCLUDE_DIR uvsrtp/uvsrtp.h)
	endif()
	if(UVOICE_UVSRTP_LIBDIR)
		find_library(UVSRTP_LIBRARY NAMES uvsrtp HINTS ${UVOICE_UVSRTP_LIBDIR}
			     NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
	else()
		find_library(UVSRTP_LIBRARY NAMES uvsrtp)
	endif()
	foreach(dep UVSRTP_INCLUDE_DIR UVSRTP_LIBRARY)
		if(NOT ${dep})
			message(FATAL_ERROR
				"${dep} not found. The css backend needs the "
				"libuvsrtp header and library. Pass "
				"-DUVOICE_UVSRTP_INCLUDEDIR=<dir with uvsrtp/uvsrtp.h> "
				"and -DUVOICE_UVSRTP_LIBDIR=<dir with libuvsrtp>.")
		endif()
	endforeach()
	list(APPEND HAL_INCLUDE_DIRS ${UVSRTP_INCLUDE_DIR})
	list(APPEND HAL_LIBRARIES ${UVSRTP_LIBRARY})
else()
	message(FATAL_ERROR
		"unknown UVOICE_HAL '${UVOICE_HAL}', use stub, udsp or css")
endif()

# The core of uvoiced: the core list, the HAL arm, trace.c, dialplan.c, the
# network services and the protocol registry.
add_library(uvoice_core OBJECT ${SOURCES})
target_include_directories(uvoice_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
			   ${UBOX_INCLUDE_DIR} ${UBUS_INCLUDE_DIR}
			   ${UCI_INCLUDE_DIR} ${HAL_INCLUDE_DIRS})
# The protocol registry of proto/proto.c lists the tables the image carries,
# one build flag per table. The SIP table lives in proto/sip/proto.c and
# the MGCP table in proto/mgcp/proto.c.
if(UVOICE_PROTO_SIP)
	target_compile_definitions(uvoice_core PRIVATE UVOICE_PROTO_SIP)
endif()
if(UVOICE_PROTO_MGCP)
	target_compile_definitions(uvoice_core PRIVATE UVOICE_PROTO_MGCP)
endif()
# the link dependencies of the backend ride the object library
target_link_libraries(uvoice_core PRIVATE ${HAL_LIBRARIES})

add_executable(uvoiced)
# pthread is for the relay mutex of trace_ring.c, which every configuration
# compiles.
target_link_libraries(uvoiced uvoice_core ${UVOICE_PROTO_OBJECTS}
		      ${UBOX_LIBRARY} ${BLOBMSG_JSON_LIBRARY} ${UBUS_LIBRARY}
		      ${UCI_LIBRARY}
		      ${UVOICE_PROTO_LIBRARIES} ${HAL_LIBRARIES} pthread)

install(TARGETS uvoiced RUNTIME DESTINATION sbin)

if(UVOICE_TESTS)
	set(UVOICE_TEST_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../test)
	if(NOT EXISTS ${UVOICE_TEST_DIR}/unit/test_sip_msg.c)
		message(FATAL_ERROR
			"UVOICE_TESTS needs the test/ tree beside src/. The OpenWrt "
			"package build copies src/ only, so point cmake -S at the "
			"src/ directory of a full uvoice source tree.")
	endif()

	enable_testing()

	# A suite that links a SIP source exists in a build that carries the SIP
	# unit, and a suite that links an MGCP source in a build with
	# UVOICE_PROTO_MGCP. Every other suite is a core suite and exists in
	# every build.

	# The parser, the typed headers, the SDP layer, the digest auth and the
	# stream framer. Each suite links uvoice_sdp and uvoice_sip, and with
	# them the public OpenSSL::Crypto link that auth.c needs.
	if(UVOICE_PROTO_SIP)
		foreach(suite test_sip_msg test_sip_hdr test_sip_auth test_sdp
			test_sip_frame)
			add_executable(${suite}
				       ${UVOICE_TEST_DIR}/unit/${suite}.c)
			target_include_directories(${suite} PRIVATE
						   ${UVOICE_TEST_DIR}/unit)
			target_link_libraries(${suite} uvoice_sdp uvoice_sip)
			add_test(NAME ${suite} COMMAND ${suite})
		endforeach()
	endif()

	# The dial plan suite compiles dialplan.c into itself. test_util.h
	# includes shared/msg.h for an inline string check that no case of the
	# suite calls, so the header alone serves it.
	add_executable(test_dialplan ${UVOICE_TEST_DIR}/unit/test_dialplan.c
		       dialplan.c)
	target_include_directories(test_dialplan PRIVATE ${UVOICE_TEST_DIR}/unit
				   ${CMAKE_CURRENT_SOURCE_DIR}
				   ${UBOX_INCLUDE_DIR})
	target_link_libraries(test_dialplan ${UBOX_LIBRARY})
	add_test(NAME test_dialplan COMMAND test_dialplan)

	if(UVOICE_PROTO_SIP)
		# These drive sip/txn.c, ua.c, reg.c, dialog.c, keepalive.c and
		# sub.c through a scripted environment: a virtual clock, a
		# capture buffer and a timer table. They need libubox for the
		# avl and nothing else.
		foreach(suite test_sip_txn test_sip_reg test_sip_dialog
			test_sip_keepalive test_sip_sub)
			add_executable(${suite}
				       ${UVOICE_TEST_DIR}/unit/${suite}.c)
			target_include_directories(${suite} PRIVATE
						   ${UVOICE_TEST_DIR}/unit)
			target_link_libraries(${suite} uvoice_sdp uvoice_sip
					      uvoice_sip_txn ${UBOX_LIBRARY})
			add_test(NAME ${suite} COMMAND ${suite})
		endforeach()

		# test_sip_transport drives real loopback datagrams.
		# test_sip_resolve feeds hand built DNS packets to the resolver
		# wire layer of uvoice_sdp and a scripted transport to its state
		# machine, and opens no socket. Both add uvoice_sip_udp, which
		# brings uloop with it, and compile trace.c for the sip trace
		# domain the transport and the resolver write.
		# proto_host_fake.c is the host the wire trace and the tap
		# endpoints reach.
		foreach(suite test_sip_transport test_sip_resolve)
			add_executable(${suite}
				       ${UVOICE_TEST_DIR}/unit/${suite}.c
				       ${UVOICE_TEST_DIR}/unit/proto_host_fake.c
				       trace.c)
			target_include_directories(${suite} PRIVATE
						   ${UVOICE_TEST_DIR}/unit)
			target_link_libraries(${suite} uvoice_sdp uvoice_sip
					      uvoice_sip_txn uvoice_sip_udp
					      ${UBOX_LIBRARY})
			add_test(NAME ${suite} COMMAND ${suite})
		endforeach()

		# The stream and TLS suites drive real loopback connections.
		# They add uvoice_sip_stream, uvoice_sip_udp for the uloop timer
		# environment, trace.c, proto_host_fake.c as the host of the
		# wire trace and the tap, and the TLS link for tls.c's OpenSSL
		# calls.
		foreach(suite test_sip_stream test_sip_tls)
			add_executable(${suite}
				       ${UVOICE_TEST_DIR}/unit/${suite}.c
				       ${UVOICE_TEST_DIR}/unit/proto_host_fake.c
				       trace.c)
			target_include_directories(${suite} PRIVATE
						   ${UVOICE_TEST_DIR}/unit)
			target_link_libraries(${suite} uvoice_sdp uvoice_sip
					      uvoice_sip_txn uvoice_sip_stream
					      uvoice_sip_udp ${UBOX_LIBRARY}
					      OpenSSL::SSL OpenSSL::Crypto)
			add_test(NAME ${suite} COMMAND ${suite})
		endforeach()

		# The committed certificate fixtures of the tls unit. The suite
		# also accepts the UVOICE_TLS_FIXTURES environment variable, so a
		# hand built binary still runs.
		foreach(suite test_sip_stream test_sip_tls)
			target_compile_definitions(${suite} PRIVATE
				UVOICE_TLS_FIXTURE_DIR="${UVOICE_TEST_DIR}/unit/tls")
		endforeach()

		# The account layer test drives the pure helpers, the fallback
		# chain, the apply class and the status map. It compiles
		# account.c and call.c into itself and links the whole SIP
		# stack, because account.c references the transport and the
		# resolver even though the pure helpers touch neither. The
		# cases also read what the core does with the requests of the
		# account layer, so proto_host_fake.c forwards the host entries
		# to the real tracker, port pool, firewall and event spawner
		# (PHF_NET_PORTS, PHF_NET_CORE), which link in with fw.c and
		# libubus.
		add_executable(test_account ${UVOICE_TEST_DIR}/unit/test_account.c
			       ${UVOICE_TEST_DIR}/unit/proto_host_fake.c
			       proto/sip/account.c proto/sip/call.c fw.c event.c
			       spawn_run.c trace.c ${UVOICE_NET_SOURCES})
		target_include_directories(test_account PRIVATE
					   ${UVOICE_TEST_DIR}/unit
					   ${CMAKE_CURRENT_SOURCE_DIR}
					   ${UBOX_INCLUDE_DIR} ${UBUS_INCLUDE_DIR})
		target_compile_definitions(test_account PRIVATE PHF_NET_PORTS
					   PHF_NET_CORE)
		target_link_libraries(test_account uvoice_sdp uvoice_sip
				      uvoice_sip_txn uvoice_sip_udp
				      uvoice_sip_stream ${UBOX_LIBRARY}
				      ${UBUS_LIBRARY} OpenSSL::SSL
				      OpenSSL::Crypto)
		add_test(NAME test_account COMMAND test_account)
	endif()

	# The netifd tracker of the core. The suite compiles iface.c and trace.c
	# into itself and drives the tracker through its three injects; it
	# connects to no ubus, but iface.c calls libubus, so the suite links it.
	add_executable(test_net_iface ${UVOICE_TEST_DIR}/unit/test_net_iface.c
		       net/iface.c trace.c)
	target_include_directories(test_net_iface PRIVATE
				   ${UVOICE_TEST_DIR}/unit
				   ${CMAKE_CURRENT_SOURCE_DIR}
				   ${UBOX_INCLUDE_DIR} ${UBUS_INCLUDE_DIR})
	target_link_libraries(test_net_iface ${UBUS_LIBRARY} ${UBOX_LIBRARY})
	add_test(NAME test_net_iface COMMAND test_net_iface)

	# The firewall reconcile of the core, over fw.c in its test mode, which
	# spawns nothing. spawn_run.c and the port pool link in for fw.c and for
	# the RTP ranges the reconcile takes from the config.
	add_executable(test_net_fw ${UVOICE_TEST_DIR}/unit/test_net_fw.c
		       net/fw_reconcile.c net/ports.c fw.c spawn_run.c trace.c)
	target_include_directories(test_net_fw PRIVATE ${UVOICE_TEST_DIR}/unit
				   ${CMAKE_CURRENT_SOURCE_DIR}
				   ${UBOX_INCLUDE_DIR})
	target_link_libraries(test_net_fw ${UBOX_LIBRARY})
	add_test(NAME test_net_fw COMMAND test_net_fw)

	# The protocol registry, in two builds of one suite: test_proto carries
	# the SIP flag alone, and test_proto_both the SIP and the MGCP flags.
	# Each compiles proto/proto.c with its flags and defines a scripted SIP
	# table itself. test_proto_both links the MGCP unit, so its MGCP table
	# is the real one, and it exists under UVOICE_PROTO_MGCP alone. The host
	# adapters of proto/proto.c reach the core services: net/, fw.c and
	# cdr.c link in, and the suite stubs the line shell, the call trace and
	# ubus. proto_host_fake.c is the recorder host the tables run on.
	set(UVOICE_PROTO_SUITES test_proto)
	if(UVOICE_PROTO_MGCP)
		list(APPEND UVOICE_PROTO_SUITES test_proto_both)
	endif()
	foreach(suite ${UVOICE_PROTO_SUITES})
		add_executable(${suite} ${UVOICE_TEST_DIR}/unit/test_proto.c
			       ${UVOICE_TEST_DIR}/unit/proto_host_fake.c
			       proto/proto.c ${UVOICE_NET_SOURCES} fw.c
			       spawn_run.c cdr.c trace.c uvoice_random.c)
		target_include_directories(${suite} PRIVATE
					   ${UVOICE_TEST_DIR}/unit
					   ${CMAKE_CURRENT_SOURCE_DIR}
					   ${UBOX_INCLUDE_DIR} ${UBUS_INCLUDE_DIR})
		target_compile_definitions(${suite} PRIVATE UVOICE_PROTO_SIP)
		target_link_libraries(${suite} ${UBUS_LIBRARY} ${UBOX_LIBRARY})
		add_test(NAME ${suite} COMMAND ${suite})
	endforeach()
	if(UVOICE_PROTO_MGCP)
		target_compile_definitions(test_proto_both PRIVATE
					   UVOICE_PROTO_MGCP)
		target_sources(test_proto_both PRIVATE dialplan.c)
		target_link_libraries(test_proto_both uvoice_proto_mgcp
				      uvoice_proto_mgcp_ep uvoice_proto_mgcp_txn
				      uvoice_proto_mgcp_text uvoice_sdp)
	endif()

	if(UVOICE_PROTO_SIP)
		# The call leg suite drives call.c through the scripted
		# environment and its own fixture for the account interface of
		# call.c, so it compiles call.c into itself (same rule as
		# test_fsm) and must not link uvoice_proto_sip or account.c: the
		# fixture defines account_call_view() itself. uvoice_sdp brings
		# the media fill the legs program streams from.
		# proto_host_fake.c is the host of the legs and reaches the real
		# port pool of net/ports.c (PHF_NET_PORTS); trace.c joins it.
		add_executable(test_call ${UVOICE_TEST_DIR}/unit/test_call.c
			       ${UVOICE_TEST_DIR}/unit/proto_host_fake.c
			       proto/sip/call.c net/ports.c trace.c)
		target_include_directories(test_call PRIVATE
					   ${UVOICE_TEST_DIR}/unit
					   ${CMAKE_CURRENT_SOURCE_DIR}
					   ${UBOX_INCLUDE_DIR})
		target_compile_definitions(test_call PRIVATE PHF_NET_PORTS)
		target_link_libraries(test_call uvoice_sdp uvoice_sip
				      uvoice_sip_txn ${UBOX_LIBRARY})
		add_test(NAME test_call COMMAND test_call)

		# The star code table and the runtime feature state are libc
		# only: they read the loaded config strings and their own state,
		# and emit no action. The suite compiles features.c into itself
		# and links the parser objects that test_util.h needs, nothing
		# else.
		add_executable(test_features
			       ${UVOICE_TEST_DIR}/unit/test_features.c
			       proto/sip/features.c)
		target_include_directories(test_features PRIVATE
					   ${UVOICE_TEST_DIR}/unit
					   ${CMAKE_CURRENT_SOURCE_DIR})
		target_link_libraries(test_features uvoice_sdp)
		add_test(NAME test_features COMMAND test_features)

		# The FSM suite checks the state matrix of the machine on the
		# virtual clock. It compiles fsm.c into itself and defines its
		# own stubs of the dialplan.h functions, so it must not link
		# dialplan.c or uvoice_proto_sip: the real matcher would collide
		# with the stubs. uvoice_sdp supplies the timer helpers and the
		# parser objects.
		add_executable(test_fsm ${UVOICE_TEST_DIR}/unit/test_fsm.c
			       proto/sip/fsm.c)
		target_include_directories(test_fsm PRIVATE
					   ${UVOICE_TEST_DIR}/unit
					   ${CMAKE_CURRENT_SOURCE_DIR}
					   ${UBOX_INCLUDE_DIR})
		target_link_libraries(test_fsm uvoice_sdp uvoice_sip
				      uvoice_sip_txn ${UBOX_LIBRARY})
		add_test(NAME test_fsm COMMAND test_fsm)

		add_executable(test_desk ${UVOICE_TEST_DIR}/unit/test_desk.c
			       proto/sip/desk.c dialplan.c)
		target_include_directories(test_desk PRIVATE
					   ${UVOICE_TEST_DIR}/unit
					   ${CMAKE_CURRENT_SOURCE_DIR}
					   ${UBOX_INCLUDE_DIR})
		target_link_libraries(test_desk uvoice_sdp uvoice_sip
				      uvoice_sip_txn ${UBOX_LIBRARY})
		add_test(NAME test_desk COMMAND test_desk)

		# The line shell suite compiles both halves of the shell
		# (line/line.c, proto/sip/line.c and proto/sip/proto.c), fsm.c
		# and dialplan.c into itself and defines its own
		# hal_backend_ops plus the whole call.h API, so it must not link
		# uvoice_proto_sip (fsm.c and call.c would collide) or the
		# backend. uvoice_sip_udp supplies txn_uloop.c for the timer
		# environment of proto/sip/line.c, and the suite compiles
		# trace.c and the core entropy source. The suite runs the real
		# uloop for bounded windows.
		add_executable(test_line ${UVOICE_TEST_DIR}/unit/test_line.c
			       line/line.c proto/sip/line.c
			       proto/sip/proto.c proto/sip/features.c
			       proto/sip/fsm.c proto/sip/desk.c dialplan.c
			       cdr.c trace.c
			       uvoice_random.c proto/proto.c net/ports.c
			       net/conntrack.c)
		target_include_directories(test_line PRIVATE
					   ${UVOICE_TEST_DIR}/unit
					   ${CMAKE_CURRENT_SOURCE_DIR}
					   ${UBOX_INCLUDE_DIR} ${UBUS_INCLUDE_DIR})
		target_compile_definitions(test_line PRIVATE UVOICE_PROTO_SIP)
		target_link_libraries(test_line uvoice_sdp uvoice_sip
				      uvoice_sip_txn uvoice_sip_udp
				      ${UBOX_LIBRARY})
		add_test(NAME test_line COMMAND test_line)
	endif()

	# The udsp backend suite compiles the whole backend into itself
	# together with the libudsp sources. The package stages only two of
	# those sources, so a build that cannot see the library's source tree
	# skips the suite.
	#
	# The manifest is explicit and mirrors udsp/src/CMakeLists.txt, list
	# for list. A glob reads whatever a stale build directory holds, so a
	# source added to the library would join this target silently or,
	# worse, not at all after a tree move. A source added there must break
	# this build.
	#
	# The rtp sources are in: core/net.c and core/stream.c call the jitter
	# buffer, the codecs and the SRTP session, so the library objects do
	# not link without them, and libsrtp2 comes with them; pkg-config
	# carries its include directory, which the rtp sources need. It must
	# not link the stub or uvoice_proto_sip: hal_backend_ops comes from
	# hal/udsp/udsp.c here. cfg/uci.c joins it because the backend loads
	# its own config.
	find_path(UDSP_TEST_SRC_DIR core/udsp.c
		  HINTS ${UVOICE_UDSP_SRCDIR}
			${CMAKE_CURRENT_SOURCE_DIR}/../../udsp/src
		  NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
	# libudsp's SRTP layer moved into libuvsrtp, compiled into the target from
	# its source tree the way the udsp sources are.
	find_path(UVSRTP_SRC_DIR uvsrtp.c
		  HINTS ${UVOICE_UVSRTP_SRCDIR}
			${CMAKE_CURRENT_SOURCE_DIR}/../../uvsrtp/src
		  NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)

	if(UDSP_TEST_SRC_DIR AND NOT UVSRTP_SRC_DIR)
		message(FATAL_ERROR
			"test_hal_udsp needs the libuvsrtp source tree; set "
			"-DUVOICE_UVSRTP_SRCDIR=<uvsrtp/src> or keep it beside udsp/")
	endif()

	if(UDSP_TEST_SRC_DIR)
		find_package(PkgConfig REQUIRED)
		pkg_check_modules(UDSP_TEST_SRTP2 REQUIRED IMPORTED_TARGET libsrtp2)
		# The suite compiles the ALSA backend of the library itself, so
		# it probes alsa-lib here, in a stub configuration too.
		find_path(ALSA_INCLUDE_DIR alsa/asoundlib.h)
		find_library(ALSA_LIBRARY NAMES asound)

		set(UDSP_TEST_SRC
		    core/engine.c core/rt.c core/udsp.c core/chan.c core/cmd.c
		    core/event.c core/thread.c core/slic_thread.c core/region_ctl.c
		    core/safe_state.c core/stream.c core/fax_plugin.c core/net.c
		    tdm/file.c tdm/alsa_raw.c
		    slic/fake.c slic/host_linux.c slic/plugin_load.c
		    dsp/g711.c dsp/g726.c dsp/cn.c dsp/vad.c
		    dsp/goertzel.c dsp/dtmf_det.c dsp/dtmf_gen.c dsp/tone_gen.c
		    dsp/gain.c dsp/fsk_mod.c dsp/cid.c dsp/fax_det.c dsp/lec.c
		    dsp/mix.c dsp/resamp.c dsp/g722.c dsp/plc.c dsp/g729.c
		    dsp/g729_tables.c
		    rtp/rtp.c rtp/rtcp.c rtp/telev.c rtp/stats.c rtp/jb.c
		    line/hook.c line/ring.c line/cidseq.c line/region.c)
		# The ALSA backend of the library, on the same probe its own
		# CMake uses, so this target compiles what the shipped libudsp
		# carries.
		if(ALSA_INCLUDE_DIR AND ALSA_LIBRARY)
			list(APPEND UDSP_TEST_SRC tdm/alsa.c)
		endif()
		list(TRANSFORM UDSP_TEST_SRC PREPEND "${UDSP_TEST_SRC_DIR}/")

		# trace_ring.c joins the backend sources because the library log
		# sink of udsp.c hands its lines to the relay, and pcapng.c
		# joins trace_ring.c for the export.
		add_executable(test_hal_udsp ${UVOICE_TEST_DIR}/unit/test_hal_udsp.c
			       hal/udsp/udsp.c hal/udsp/udsp_events.c
			       hal/hal_relay.c hal/hal_media.c
			       cfg/uci.c dialplan.c trace.c trace_ring.c pcapng.c
			       ${UVSRTP_SRC_DIR}/uvsrtp.c
			       ${UDSP_TEST_SRC})
		target_include_directories(test_hal_udsp PRIVATE
					   ${UVOICE_TEST_DIR}/unit
					   ${CMAKE_CURRENT_SOURCE_DIR}
					   ${UBOX_INCLUDE_DIR} ${UCI_INCLUDE_DIR}
					   ${UDSP_TEST_SRC_DIR}/include
					   ${UVSRTP_SRC_DIR}/include
					   ${UDSP_TEST_SRC_DIR}
					   ${UDSP_TEST_SRC_DIR}/core
					   ${UDSP_TEST_SRC_DIR}/dsp
					   ${UDSP_TEST_SRC_DIR}/line
					   ${UDSP_TEST_SRC_DIR}/slic)
		# The region files are the udsp package's, and the suite reads
		# them from the source tree the way udsp's test_core does.
		# libudsp core is compiled into this target from the source tree, so
		# its UDSP_VERSION comes from udsp's own Makefile, not uvoice's.
		file(STRINGS "${UDSP_TEST_SRC_DIR}/../Makefile" _udsp_pkgver
		     REGEX "^PKG_VERSION:=")
		string(REGEX REPLACE "^PKG_VERSION:=" "" _udsp_ver "${_udsp_pkgver}")
		string(STRIP "${_udsp_ver}" _udsp_ver)
		set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
			     "${UDSP_TEST_SRC_DIR}/../Makefile")
		target_compile_definitions(test_hal_udsp PRIVATE UVOICE_HAL_UDSP
			UDSP_VERSION="${_udsp_ver}"
			UDSP_REGION_DIR="${UDSP_TEST_SRC_DIR}/../data/regions")
		target_link_libraries(test_hal_udsp ${UCI_LIBRARY} ${UBOX_LIBRARY}
				      PkgConfig::UDSP_TEST_SRTP2 m pthread dl)
		# The arm order of the canceller capture shows only at the call
		# itself, so the suite wraps that library entry and records what
		# the ring held when the bit rose.
		target_link_options(test_hal_udsp PRIVATE
				    -Wl,--wrap=udsp_lec_capture_enable)
		if(ALSA_INCLUDE_DIR AND ALSA_LIBRARY)
			target_compile_definitions(test_hal_udsp PRIVATE HAVE_ALSA)
			target_include_directories(test_hal_udsp PRIVATE
						   ${ALSA_INCLUDE_DIR})
			target_link_libraries(test_hal_udsp ${ALSA_LIBRARY})
		endif()
		add_test(NAME test_hal_udsp COMMAND test_hal_udsp)

		# The end-to-end entry of the canceller ring replays what
		# test_hal_udsp exported through the canceller code that
		# test_hal_udsp compiled, so lec_replay comes from the same
		# tree, the way udsp/src/CMakeLists.txt builds it.
		add_executable(lec_replay
			       ${UDSP_TEST_SRC_DIR}/test/bench/lec_replay.c
			       ${UDSP_TEST_SRC_DIR}/dsp/lec.c
			       ${UDSP_TEST_SRC_DIR}/dsp/cn.c)
		target_include_directories(lec_replay PRIVATE
					   ${UDSP_TEST_SRC_DIR}/include
					   ${UDSP_TEST_SRC_DIR}/dsp)
		target_link_libraries(lec_replay m)
		set(UVOICE_LEC_E2E_PCAP
		    ${CMAKE_CURRENT_BINARY_DIR}/lec-e2e-rings.pcapng)
		# Without udebug the export case skips and writes no capture,
		# so the entry must skip too and not read an absent file as a
		# failure.
		if(HAVE_UDEBUG_H)
			set(UVOICE_LEC_E2E_ARGS ${UVOICE_LEC_E2E_PCAP}
			    ${UDSP_TEST_SRC_DIR}/tools/lec-capture.py
			    $<TARGET_FILE:lec_replay>)
		else()
			set(UVOICE_LEC_E2E_ARGS --skip
			    "the build carries no udebug support")
		endif()
		set_tests_properties(test_hal_udsp PROPERTIES
			FIXTURES_SETUP lec_capture_e2e
			ENVIRONMENT
			"UVOICE_LEC_CAPTURE_EXPORT=${UVOICE_LEC_E2E_PCAP}")
	else()
		message(STATUS "libudsp sources not found, skipping "
			"test_hal_udsp. Pass -DUVOICE_UDSP_SRCDIR=<udsp/src> "
			"to build it.")
	endif()

	# Registered in every configuration: without the udsp tree the script
	# gets no extractor and no replay, and it answers a skip.
	add_test(NAME lec_capture_e2e
		 COMMAND sh ${UVOICE_TEST_DIR}/tools/lec_capture_e2e.sh
			 ${UVOICE_LEC_E2E_ARGS})
	set_tests_properties(lec_capture_e2e PROPERTIES
		FIXTURES_REQUIRED lec_capture_e2e SKIP_RETURN_CODE 77)

	# The stub backend suite compiles the whole stub into itself and drives
	# hal_backend_ops, the way test_hal_udsp drives the udsp backend. It
	# must not link uvoice_proto_sip or another backend: hal_backend_ops comes
	# from hal/stub/stub.c here. It builds in every configuration, because
	# the stub needs no DSP header and no library beside the ones uvoiced
	# already carries. libubus is linked for the voice.stub object the
	# backend defines; the suite attaches no object and needs no fake for
	# it. trace.c serves the backend's TRACE lines. The parser objects stay
	# out, as they do for test_hal_udsp. test_util.h includes shared/msg.h
	# for its string check and this suite calls none, so the src directory
	# on the include path is all it needs.
	add_executable(test_hal_stub ${UVOICE_TEST_DIR}/unit/test_hal_stub.c
		       hal/stub/stub.c trace.c)
	target_include_directories(test_hal_stub PRIVATE ${UVOICE_TEST_DIR}/unit
				   ${CMAKE_CURRENT_SOURCE_DIR}
				   ${UBOX_INCLUDE_DIR} ${UBUS_INCLUDE_DIR})
	target_compile_definitions(test_hal_stub PRIVATE UVOICE_HAL_STUB)
	target_link_libraries(test_hal_stub ${UBOX_LIBRARY} ${UBUS_LIBRARY})
	add_test(NAME test_hal_stub COMMAND test_hal_stub)

	# The css backend behind a fake /dev/css-voice and fake evdev inputs. It
	# compiles the backend, the shared relay and media helpers and libuvsrtp
	# from source, the way test_hal_udsp does, and drives hal_backend_ops. It
	# builds when the libuvsrtp source, libsrtp2 and alsa-lib are present.
	pkg_check_modules(CSS_TEST_SRTP2 IMPORTED_TARGET libsrtp2)
	pkg_check_modules(CSS_TEST_ALSA IMPORTED_TARGET alsa)
	if(UVSRTP_SRC_DIR AND CSS_TEST_SRTP2_FOUND AND CSS_TEST_ALSA_FOUND)
		add_executable(test_hal_css ${UVOICE_TEST_DIR}/unit/test_hal_css.c
			       hal/css/css.c hal/css/css_media.c hal/css/css_line.c
			       hal/css/css_input.c hal/css/css_audio.c
			       hal/css/css_tone.c hal/hal_relay.c hal/hal_media.c
			       trace.c ${UVSRTP_SRC_DIR}/uvsrtp.c)
		target_include_directories(test_hal_css PRIVATE
					   ${UVOICE_TEST_DIR}/unit
					   ${CMAKE_CURRENT_SOURCE_DIR}
					   ${UBOX_INCLUDE_DIR}
					   ${UVSRTP_SRC_DIR}/include)
		target_compile_definitions(test_hal_css PRIVATE UVOICE_HAL_CSS)
		target_link_libraries(test_hal_css ${UBOX_LIBRARY}
				      PkgConfig::CSS_TEST_SRTP2 PkgConfig::CSS_TEST_ALSA
				      m pthread)
		add_test(NAME test_hal_css COMMAND test_hal_css)
	else()
		message(STATUS "test_hal_css skipped: needs the libuvsrtp source, "
			"libsrtp2 and alsa-lib")
	endif()

	# The event publisher builds argv records and spawns the platform
	# script. Most of the suite drives the no-spawn test mode of event.c
	# and starts no child; the closing classes drive the real spawn, which
	# is why spawn_run.c links in.
	add_executable(test_event ${UVOICE_TEST_DIR}/unit/test_event.c event.c
		       spawn_run.c trace.c)
	target_include_directories(test_event PRIVATE ${UVOICE_TEST_DIR}/unit
				   ${CMAKE_CURRENT_SOURCE_DIR}
				   ${UBOX_INCLUDE_DIR})
	target_link_libraries(test_event ${UBOX_LIBRARY})
	add_test(NAME test_event COMMAND test_event)

	# The CDR ring and the per-line counters are libubox-blobmsg plus libc.
	# The suite compiles cdr.c and trace.c into itself.
	add_executable(test_cdr ${UVOICE_TEST_DIR}/unit/test_cdr.c cdr.c trace.c)
	target_include_directories(test_cdr PRIVATE ${UVOICE_TEST_DIR}/unit
				   ${CMAKE_CURRENT_SOURCE_DIR}
				   ${UBOX_INCLUDE_DIR})
	target_link_libraries(test_cdr ${UBOX_LIBRARY})
	add_test(NAME test_cdr COMMAND test_cdr)

	# The ring suite drives the mask split of trace.c, the rings and the
	# relay of trace_ring.c and the writer of pcapng.c, and reads the
	# exported file back with a parser of its own. It compiles the three
	# units into itself and links libubox for uloop and udebug plus pthread
	# for the relay's second thread. The parser objects stay out, as they
	# do for test_hal_stub.
	add_executable(test_trace_ring ${UVOICE_TEST_DIR}/unit/test_trace_ring.c
		       trace.c trace_ring.c pcapng.c)
	target_include_directories(test_trace_ring PRIVATE
				   ${UVOICE_TEST_DIR}/unit
				   ${CMAKE_CURRENT_SOURCE_DIR}
				   ${UBOX_INCLUDE_DIR})
	target_link_libraries(test_trace_ring ${UBOX_LIBRARY} pthread)
	add_test(NAME test_trace_ring COMMAND test_trace_ring)

	# The controller suite compiles calltrace.c with the real rings and the
	# real pcapng writer, and defines every renderer and getter the
	# controller reaches outside them, so it must not link uvoice_proto_sip,
	# account.c, status.c, cdr.c or a backend: each of those would collide
	# with a fixture. It links the blobmsg json renderer beside libubox,
	# because a snapshot and a manifest are JSON text, and pthread for the
	# relay of trace_ring.c. The parser objects stay out, as they do for
	# test_trace_ring.
	add_executable(test_calltrace ${UVOICE_TEST_DIR}/unit/test_calltrace.c
		       calltrace.c trace.c trace_ring.c pcapng.c)
	target_include_directories(test_calltrace PRIVATE
				   ${UVOICE_TEST_DIR}/unit
				   ${CMAKE_CURRENT_SOURCE_DIR}
				   ${UBOX_INCLUDE_DIR})
	target_link_libraries(test_calltrace ${UBOX_LIBRARY}
			      ${BLOBMSG_JSON_LIBRARY} pthread)
	add_test(NAME test_calltrace COMMAND test_calltrace)

	# The config loader suite writes temp config files and reads them back
	# through libuci via UVOICE_UCI_CONFDIR. It compiles the loader and
	# trace.c into itself; no other suite links libuci.
	add_executable(test_cfg ${UVOICE_TEST_DIR}/unit/test_cfg.c cfg/uci.c
		       dialplan.c trace.c)
	target_include_directories(test_cfg PRIVATE ${UVOICE_TEST_DIR}/unit
				   ${CMAKE_CURRENT_SOURCE_DIR}
				   ${UBOX_INCLUDE_DIR} ${UCI_INCLUDE_DIR})
	target_link_libraries(test_cfg ${UCI_LIBRARY} ${UBOX_LIBRARY})
	add_test(NAME test_cfg COMMAND test_cfg)

	# test_dispatch compiles the dispatch unit into itself and defines the
	# two main.c hooks plus its own uv, so it links libubox alone.
	add_executable(test_dispatch ${UVOICE_TEST_DIR}/unit/test_dispatch.c
		       event_dispatch.c trace.c)
	target_include_directories(test_dispatch PRIVATE ${UVOICE_TEST_DIR}/unit
				   ${CMAKE_CURRENT_SOURCE_DIR}
				   ${UBOX_INCLUDE_DIR})
	target_link_libraries(test_dispatch ${UBOX_LIBRARY})
	add_test(NAME test_dispatch COMMAND test_dispatch)

	# test_ubus embeds ubus.c whole (#include of the source) and stubs the
	# daemon around it. Its JSON assertion uses libblobmsg_json, which the
	# daemon links too and which is probed beside the other libraries.
	add_executable(test_ubus ${UVOICE_TEST_DIR}/unit/test_ubus.c)
	target_include_directories(test_ubus PRIVATE ${UVOICE_TEST_DIR}/unit
				   ${CMAKE_CURRENT_SOURCE_DIR}
				   ${UBOX_INCLUDE_DIR} ${UBUS_INCLUDE_DIR})
	target_link_libraries(test_ubus ${UBUS_LIBRARY} ${UBOX_LIBRARY}
			      ${BLOBMSG_JSON_LIBRARY})
	add_test(NAME test_ubus COMMAND test_ubus)

	# The SIPp scenario lint builds nothing. A nop behind a checking message
	# makes a scenario report a pass it never earned. It runs through sh, so
	# a checkout that lost the execute bit still runs it. The out-of-call
	# scenarios of the e2e harness are scenarios too, so it reads that
	# directory as well.
	add_test(NAME sipp_scenario_lint
		 COMMAND sh ${UVOICE_TEST_DIR}/tools/sipp_scenario_lint.sh
			 ${UVOICE_TEST_DIR}/sipp ${UVOICE_TEST_DIR}/e2e)

	# The fuzz targets compile the code under test into themselves instead of
	# reusing the object libraries, so the sanitizer flags cover it.
	foreach(fuzzer fuzz_sip_msg fuzz_sdp)
		add_executable(${fuzzer} ${UVOICE_TEST_DIR}/fuzz/${fuzzer}.c
			       proto/shared/msg.c proto/shared/hdr.c)
		target_include_directories(${fuzzer} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
		if(CMAKE_C_COMPILER_ID MATCHES "Clang")
			target_compile_definitions(${fuzzer} PRIVATE UVOICE_FUZZ_LIBFUZZER)
			target_compile_options(${fuzzer} PRIVATE -fsanitize=fuzzer,address)
			target_link_options(${fuzzer} PRIVATE -fsanitize=fuzzer,address)
		endif()
	endforeach()
	target_sources(fuzz_sdp PRIVATE proto/shared/sdp.c)

	# The MGCP suites. test_mgcp_txn drives the transaction layer on a
	# virtual clock. uvoice_sdp brings the text helpers, the protocol timer
	# and the libubox link of its uloop binding. The fuzz target follows the
	# rule of the fuzz targets above.
	if(UVOICE_PROTO_MGCP)
		add_executable(test_mgcp_msg
			       ${UVOICE_TEST_DIR}/unit/test_mgcp_msg.c)
		target_include_directories(test_mgcp_msg PRIVATE
					   ${UVOICE_TEST_DIR}/unit)
		target_link_libraries(test_mgcp_msg uvoice_proto_mgcp_text
				      uvoice_sdp)
		add_test(NAME test_mgcp_msg COMMAND test_mgcp_msg)

		add_executable(test_mgcp_txn
			       ${UVOICE_TEST_DIR}/unit/test_mgcp_txn.c)
		target_include_directories(test_mgcp_txn PRIVATE
					   ${UVOICE_TEST_DIR}/unit)
		target_link_libraries(test_mgcp_txn uvoice_proto_mgcp_txn
				      uvoice_proto_mgcp_text uvoice_sdp)
		add_test(NAME test_mgcp_txn COMMAND test_mgcp_txn)

		# test_mgcp_ep drives the endpoints over the real transaction
		# layer on the virtual clock, with a capture send. It compiles
		# dialplan.c, the port pool of net/ports.c (PHF_NET_PORTS) and
		# trace.c beside proto_host_fake.c, the host of the endpoints.
		add_executable(test_mgcp_ep
			       ${UVOICE_TEST_DIR}/unit/test_mgcp_ep.c
			       ${UVOICE_TEST_DIR}/unit/proto_host_fake.c
			       dialplan.c net/ports.c trace.c)
		target_include_directories(test_mgcp_ep PRIVATE
					   ${UVOICE_TEST_DIR}/unit
					   ${CMAKE_CURRENT_SOURCE_DIR}
					   ${UBOX_INCLUDE_DIR})
		target_compile_definitions(test_mgcp_ep PRIVATE PHF_NET_PORTS)
		target_link_libraries(test_mgcp_ep uvoice_proto_mgcp_ep
				      uvoice_proto_mgcp_txn
				      uvoice_proto_mgcp_text uvoice_sdp
				      ${UBOX_LIBRARY})
		add_test(NAME test_mgcp_ep COMMAND test_mgcp_ep)

		add_executable(fuzz_mgcp_msg
			       ${UVOICE_TEST_DIR}/fuzz/fuzz_mgcp_msg.c
			       proto/mgcp/msg.c proto/shared/msg.c
			       proto/shared/hdr.c)
		target_include_directories(fuzz_mgcp_msg PRIVATE
					   ${CMAKE_CURRENT_SOURCE_DIR})
		if(CMAKE_C_COMPILER_ID MATCHES "Clang")
			target_compile_definitions(fuzz_mgcp_msg PRIVATE
						   UVOICE_FUZZ_LIBFUZZER)
			target_compile_options(fuzz_mgcp_msg PRIVATE
					       -fsanitize=fuzzer,address)
			target_link_options(fuzz_mgcp_msg PRIVATE
					    -fsanitize=fuzzer,address)
		endif()
	endif()
endif()
