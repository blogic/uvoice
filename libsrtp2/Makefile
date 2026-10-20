#
# Copyright (C) 2026 Adtran Inc.
#
# SPDX-License-Identifier: BSD-2-Clause
#
include $(TOPDIR)/rules.mk

PKG_NAME:=libsrtp2
PKG_VERSION:=2.7.0
PKG_RELEASE:=1

PKG_SOURCE:=libsrtp-$(PKG_VERSION).tar.gz
PKG_SOURCE_URL:=https://github.com/cisco/libsrtp/archive/refs/tags/
PKG_SOURCE_URL_FILE:=v$(PKG_VERSION).tar.gz
PKG_HASH:=54facb1727a557c2a76b91194dcb2d0a453aaf8e2d0cbbf1e3c2848c323e28ad
PKG_BUILD_DIR:=$(BUILD_DIR)/libsrtp-$(PKG_VERSION)

PKG_LICENSE:=BSD-3-Clause
PKG_LICENSE_FILES:=LICENSE
PKG_CPE_ID:=cpe:/a:cisco:libsrtp
PKG_MAINTAINER:=Chad Monroe <chad.monroe@adtran.com>
PKG_ABI_VERSION:=1
PKG_BUILD_PARALLEL:=1
PKG_INSTALL:=1

include $(INCLUDE_DIR)/package.mk

# Compiled-in debug logging would print cipher and authentication keys.
CONFIGURE_ARGS += \
	--disable-openssl \
	--disable-nss \
	--disable-debug-logging

MAKE_FLAGS += libsrtp2.a shared_library

define Package/libsrtp2
  SECTION:=libs
  CATEGORY:=Libraries
  TITLE:=Secure Real-time Transport Protocol library
  URL:=https://github.com/cisco/libsrtp
  ABI_VERSION:=$(PKG_ABI_VERSION)
endef

define Package/libsrtp2/description
  Cisco libSRTP provides Secure Real-time Transport Protocol processing.
endef

define Build/InstallDev
	$(INSTALL_DIR) $(1)/usr/include/srtp2 $(1)/usr/lib/pkgconfig
	$(CP) $(PKG_INSTALL_DIR)/usr/include/srtp2/*.h $(1)/usr/include/srtp2/
	$(CP) $(PKG_INSTALL_DIR)/usr/lib/libsrtp2.a $(1)/usr/lib/
	$(CP) $(PKG_INSTALL_DIR)/usr/lib/libsrtp2.so* $(1)/usr/lib/
	$(CP) $(PKG_INSTALL_DIR)/usr/lib/pkgconfig/libsrtp2.pc \
		$(1)/usr/lib/pkgconfig/
endef

define Package/libsrtp2/install
	$(INSTALL_DIR) $(1)/usr/lib
	$(CP) $(PKG_INSTALL_DIR)/usr/lib/libsrtp2.so.$(PKG_ABI_VERSION)* \
		$(1)/usr/lib/
endef

$(eval $(call BuildPackage,libsrtp2))
