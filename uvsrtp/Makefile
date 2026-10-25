# SPDX-License-Identifier: LGPL-2.1-only
#
# uvsrtp - SRTP/SRTCP per-stream protection over libsrtp2
# Copyright (C) 2026 Adtran Inc.
# Author: Chad Monroe <chad.monroe@adtran.com>
include $(TOPDIR)/rules.mk

PKG_NAME:=uvsrtp
PKG_VERSION:=1.0.0
PKG_RELEASE:=1
PKG_LICENSE:=LGPL-2.1-only
PKG_LICENSE_FILES:=LICENSE
PKG_MAINTAINER:=Chad Monroe <chad.monroe@adtran.com>

PKG_BUILD_DEPENDS:=libsrtp2
CMAKE_INSTALL:=1

include $(INCLUDE_DIR)/package.mk
include $(INCLUDE_DIR)/cmake.mk

CMAKE_SOURCE_SUBDIR:=src
# PKG_VERSION is the single source of the SONAME; src/CMakeLists.txt requires it.
CMAKE_OPTIONS += -DUVSRTP_VERSION=$(PKG_VERSION)

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src $(PKG_BUILD_DIR)/
	$(Build/Patch)
endef

define Package/libuvsrtp
  SECTION:=libs
  CATEGORY:=Libraries
  SUBMENU:=Telephony
  TITLE:=SRTP/SRTCP per-stream protection over libsrtp2
  DEPENDS:=+libsrtp2
  ABI_VERSION:=$(PKG_VERSION)
endef

define Package/libuvsrtp/description
  A small library that protects and unprotects one RTP/RTCP stream with
  AES-CM-128 and HMAC-SHA1, over Cisco libsrtp2. One context per stream, with
  create, rekey, protect, unprotect and counters.
endef

define Build/InstallDev
	$(INSTALL_DIR) $(1)/usr/include/uvsrtp $(1)/usr/lib/pkgconfig
	$(CP) $(PKG_BUILD_DIR)/src/include/uvsrtp/uvsrtp.h \
		$(1)/usr/include/uvsrtp/
	$(CP) $(PKG_INSTALL_DIR)/usr/lib/libuvsrtp.so* $(1)/usr/lib/
	$(CP) $(PKG_INSTALL_DIR)/usr/lib/pkgconfig/uvsrtp.pc \
		$(1)/usr/lib/pkgconfig/
endef

define Package/libuvsrtp/install
	$(INSTALL_DIR) $(1)/usr/lib
	$(CP) $(PKG_INSTALL_DIR)/usr/lib/libuvsrtp.so.* $(1)/usr/lib/
endef

$(eval $(call BuildPackage,libuvsrtp))
