# SPDX-License-Identifier: LGPL-2.1-only
#
# udsp - software voice DSP for FXS lines
# Copyright (C) 2026 Adtran Inc.
# Author: Chad Monroe <chad.monroe@adtran.com>
include $(TOPDIR)/rules.mk

PKG_NAME:=udsp
PKG_VERSION:=1.1.4
PKG_RELEASE:=1
PKG_LICENSE:=LGPL-2.1-only BSD-2-Clause
PKG_LICENSE_FILES:=LICENSE LICENSES/BSD-2-Clause.txt
PKG_MAINTAINER:=Chad Monroe <chad.monroe@adtran.com>

PKG_BUILD_DEPENDS:=alsa-lib libsrtp2 uvsrtp UDSP_OPUS:libopus
CMAKE_INSTALL:=1
# Every config symbol CMAKE_OPTIONS or DEPENDS reads, so a change of one
# reconfigures the package.
PKG_CONFIG_DEPENDS:=CONFIG_UDSP_OPUS

include $(INCLUDE_DIR)/package.mk
include $(INCLUDE_DIR)/cmake.mk

CMAKE_SOURCE_SUBDIR:=src
CMAKE_OPTIONS += -DUDSP_TOOLS=ON
# ON or OFF, never AUTO: a staging-dir libopus left by another package must
# not flip the link under a config that did not select it.
CMAKE_OPTIONS += -DUDSP_OPUS=$(if $(CONFIG_UDSP_OPUS),ON,OFF)
# PKG_VERSION is the single source of the banner; core/udsp.c requires it at
# compile time.
CMAKE_OPTIONS += -DUDSP_VERSION=$(PKG_VERSION)

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src $(PKG_BUILD_DIR)/
	$(CP) ./data $(PKG_BUILD_DIR)/
	# Future patches need a/src/ paths because Build/Patch applies -p1 at
	# PKG_BUILD_DIR, with sources under src/.
	$(Build/Patch)
endef

define Package/libudsp
  SECTION:=net
  CATEGORY:=Network
  SUBMENU:=Telephony
  TITLE:=udsp software voice DSP for FXS lines
  DEPENDS:=+alsa-lib +libsrtp2 +libuvsrtp +UDSP_OPUS:libopus
  ABI_VERSION:=$(PKG_VERSION)
endef

define Package/libudsp/config
config UDSP_OPUS
	bool "Build the Opus codec into libudsp"
	depends on PACKAGE_libudsp
	default n
	help
	  Compiles dsp/opus.c against libopus and advertises the Opus
	  selector. Off by default; images without it carry no libopus.
endef

define Package/libudsp/description
  A shared library with a software voice DSP for FXS lines: line control,
  codecs, RTP/SRTP, jitter buffer, tone/DTMF/CID generation and detection, and
  echo cancellation, behind a plug-in HAL. Hosted in-process by uvoiced through
  its hal/udsp backend. The SLIC control plug-in is a separate package
  (udsp-slic-proslic).
endef

define Package/udsp-cli
  SECTION:=net
  CATEGORY:=Network
  SUBMENU:=Telephony
  TITLE:=udsp lab and bring-up tool
  DEPENDS:=+libudsp
endef

define Package/udsp-cli/description
  Bring-up and lab tools for libudsp. udsp-cli: loopback, tone, ring, cid,
  dtmf and a call to a fixed RTP peer, the line-level lab tool.
  udsp-linecap: one line action while the SLIC transmit path is
  recorded to a file, for the host-side analysis of what the line carried.
endef

define Build/InstallDev
	$(INSTALL_DIR) $(1)/usr/include/udsp $(1)/usr/lib \
		$(1)/usr/share/udsp/src
	$(CP) $(PKG_BUILD_DIR)/src/include/udsp/*.h $(1)/usr/include/udsp/
	$(CP) $(PKG_BUILD_DIR)/src/slic/host_linux.h $(1)/usr/include/udsp/
	$(CP) $(PKG_BUILD_DIR)/src/slic/plugin_load.h $(1)/usr/include/udsp/
	$(CP) $(PKG_BUILD_DIR)/src/slic/fake.h $(1)/usr/include/udsp/
	$(CP) $(PKG_INSTALL_DIR)/usr/lib/libudsp.so* $(1)/usr/lib/
	$(CP) $(PKG_BUILD_DIR)/src/slic/host_linux.c \
		$(PKG_BUILD_DIR)/src/slic/plugin_load.c $(1)/usr/share/udsp/src/
endef

define Package/libudsp/install
	$(INSTALL_DIR) $(1)/usr/lib
	$(CP) $(PKG_INSTALL_DIR)/usr/lib/libudsp.so.* $(1)/usr/lib/
	$(INSTALL_DIR) $(1)/usr/share/udsp/regions
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/data/regions/*.conf \
		$(1)/usr/share/udsp/regions/
endef

define Package/udsp-cli/install
	$(INSTALL_DIR) $(1)/usr/bin
	$(INSTALL_BIN) $(PKG_INSTALL_DIR)/usr/bin/udsp-cli $(1)/usr/bin/udsp-cli
	$(INSTALL_BIN) $(PKG_INSTALL_DIR)/usr/bin/udsp-linecap \
		$(1)/usr/bin/udsp-linecap
endef

$(eval $(call BuildPackage,libudsp))
$(eval $(call BuildPackage,udsp-cli))
