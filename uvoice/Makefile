# SPDX-License-Identifier: GPL-2.0-only
#
# uvoice - native voice stack for OpenWrt
# Copyright (C) 2026 Adtran Inc.
# Author: Chad Monroe <chad.monroe@adtran.com>
#
# Licensed under GPL-2.0-only with the linking exception stated in LICENSE.

include $(TOPDIR)/rules.mk

PKG_NAME:=uvoice
PKG_VERSION:=1.0.6
PKG_RELEASE:=1

PKG_LICENSE:=GPL-2.0-only
PKG_LICENSE_FILES:=LICENSE
PKG_MAINTAINER:=Chad Monroe <chad.monroe@adtran.com>
PKG_BUILD_PARALLEL:=1
CMAKE_INSTALL:=1
# Every config symbol CMAKE_OPTIONS or DEPENDS reads, so a change of one
# reconfigures the package.
PKG_CONFIG_DEPENDS:= \
	CONFIG_UVOICE_HAL_UDSP \
	CONFIG_UVOICE_T38 \
	CONFIG_UVOICE_PROTO_SIP \
	CONFIG_UVOICE_PROTO_MGCP

include $(INCLUDE_DIR)/package.mk
include $(INCLUDE_DIR)/cmake.mk

# The HAL backend is a build time choice between stub and udsp, see
# Package/uvoice/config. The default choice is stub. The stub backend needs no
# hardware and no library beside the ones uvoiced always links. The udsp backend
# links the shared library libudsp, whose package stages its headers and
# libudsp.so, so it needs no path variable inside a buildroot. libudsp carries
# its own library dependencies. A build outside a buildroot passes
# UVOICE_UDSP_INCLUDEDIR and UVOICE_UDSP_LIBDIR to cmake.
CMAKE_OPTIONS += -DUVOICE_HAL=$(if $(CONFIG_UVOICE_HAL_UDSP),udsp,stub)
# The signaling protocols the image carries. The SIP entry prompts only beside
# MGCP, and the cmake configure refuses an image with no protocol unit.
CMAKE_OPTIONS += -DUVOICE_PROTO_SIP=$(if $(CONFIG_UVOICE_PROTO_SIP),ON,OFF)
CMAKE_OPTIONS += -DUVOICE_PROTO_MGCP=$(if $(CONFIG_UVOICE_PROTO_MGCP),ON,OFF)
# PKG_VERSION is the single source; uvoice.h requires UVOICE_VERSION at compile
# time.
CMAKE_OPTIONS += -DUVOICE_VERSION=$(PKG_VERSION)
# The package build copies src/ alone, so the schema version the call trace
# manifest names is read here from the document and passed the same way.
SCHEMA_VERSION:=$(shell sed -n 's/^  "version": "\([0-9.]*\)".*/\1/p' ./schema/uvoice-api-schema.json)
CMAKE_OPTIONS += -DUVOICE_SCHEMA_VERSION=$(SCHEMA_VERSION)

define Package/uvoice
  SECTION:=net
  CATEGORY:=Network
  SUBMENU:=Telephony
  TITLE:=Native voice stack for OpenWrt
  DEPENDS:=+libubox +libubus +libuci +libblobmsg-json \
	+UVOICE_PROTO_SIP:libopenssl \
	+curl \
	+jq \
	+tcpdump \
	+ucode-mod-struct \
	+ucode \
	+ucode-mod-ubus \
	+UVOICE_HAL_UDSP:libudsp \
	+UVOICE_HAL_UDSP:alsa-utils \
	+UVOICE_T38:udsp-fax-spandsp
endef

define Package/uvoice/description
  Lightweight voice stack for the MediaTek MT798x and Airoha AN7581
  FXS platforms. One libubox and libubus daemon contains the SIP user
  agents or the MGCP endpoints, FXS state machines and the DSP control
  plane. The hardware backend and the protocols are selected at build
  time.
endef

define Package/uvoice/config
	choice
		prompt "uvoice HAL backend"
		default UVOICE_HAL_STUB

		config UVOICE_HAL_STUB
			bool "stub: no hardware, x86 and CI"

		config UVOICE_HAL_UDSP
			bool "udsp: software voice DSP on ALSA and spidev"
	endchoice

	config UVOICE_T38
		bool "T.38 fax relay through the spandsp plug-in"
		depends on UVOICE_HAL_UDSP
		default n
		help
		  Adds the udsp-fax-spandsp package, the spandsp T.38 gateway
		  plug-in, to the image. T.38 relay also needs fax_t38_enable
		  on the channel and a plug-in that loads. Off means G.711 fax
		  passthrough only.

	config UVOICE_PROTO_SIP
		bool "SIP signaling" if UVOICE_PROTO_MGCP
		default y
		help
		  Puts the SIP protocol unit and its OpenSSL link in the image.
		  voip.base.protocol sip selects it at the daemon start. The
		  prompt shows only while UVOICE_PROTO_MGCP is set; without it,
		  the entry keeps its default, so an image always carries a
		  protocol.

	config UVOICE_PROTO_MGCP
		bool "MGCP signaling (RFC 3435)"
		default n
		help
		  Puts the MGCP protocol unit in the image, the gateway side
		  of RFC 3435 and PacketCable NCS 1.5. voip.base.protocol mgcp
		  selects it at the daemon start. The unit links no OpenSSL,
		  so an image with MGCP alone carries no libopenssl. With
		  this option set, UVOICE_PROTO_SIP can be turned off.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Package/uvoice/install
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) $(PKG_INSTALL_DIR)/usr/sbin/uvoiced $(1)/usr/sbin/uvoiced
	$(INSTALL_BIN) ./files/uvoice_tls_cert.sh $(1)/usr/sbin/uvoice_tls_cert.sh

	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./files/uvoice.init $(1)/etc/init.d/uvoice

	$(INSTALL_DIR) $(1)/usr/srg/scripts
	$(INSTALL_BIN) ./files/voice-cli $(1)/usr/srg/scripts/voice-cli
	$(INSTALL_BIN) ./files/uvoice-defaults.sh $(1)/usr/srg/scripts/uvoice-defaults.sh
	$(INSTALL_DATA) ./files/uvoice.sh $(1)/usr/srg/scripts/uvoice.sh

	$(INSTALL_DIR) $(1)/usr/share/uvoice
	$(INSTALL_BIN) ./files/uvoice-cli.uc $(1)/usr/share/uvoice/uvoice-cli.uc
	$(INSTALL_BIN) ./files/calltrace-wire.sh $(1)/usr/share/uvoice/calltrace-wire.sh
	$(INSTALL_BIN) ./files/calltrace-finish.sh $(1)/usr/share/uvoice/calltrace-finish.sh
	$(INSTALL_DATA) ./files/calltrace-README.txt $(1)/usr/share/uvoice/calltrace-README.txt
	$(INSTALL_DATA) ./files/calltrace-summary.uc $(1)/usr/share/uvoice/calltrace-summary.uc

	$(INSTALL_DIR) $(1)/usr/srg/scripts/techlog
	$(INSTALL_BIN) ./files/techlog-uvoice.sh $(1)/usr/srg/scripts/techlog/techlog-uvoice.sh
endef

$(eval $(call BuildPackage,uvoice))
