# SPDX-License-Identifier: LGPL-2.1-only
#
# uvtransport - ICE and DTLS-SRTP media transport for one stream
# Copyright (C) 2026 John Crispin <john@phrozen.org>
include $(TOPDIR)/rules.mk

PKG_NAME:=uvtransport
PKG_VERSION:=1.0.0
PKG_RELEASE:=1
PKG_LICENSE:=LGPL-2.1-only
PKG_LICENSE_FILES:=LICENSE
PKG_MAINTAINER:=John Crispin <john@phrozen.org>

PKG_BUILD_DEPENDS:=libjuice libuvsrtp libopenssl
CMAKE_INSTALL:=1

include $(INCLUDE_DIR)/package.mk
include $(INCLUDE_DIR)/cmake.mk

CMAKE_SOURCE_SUBDIR:=src
CMAKE_OPTIONS += -DUVTRANSPORT_VERSION=$(PKG_VERSION)

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src $(PKG_BUILD_DIR)/
	$(Build/Patch)
endef

define Package/libuvtransport
  SECTION:=libs
  CATEGORY:=Libraries
  SUBMENU:=Telephony
  TITLE:=ICE and DTLS-SRTP media transport for one stream
  DEPENDS:=+libjuice +libuvsrtp +libopenssl +libpthread
  ABI_VERSION:=$(PKG_VERSION)
endef

define Package/libuvtransport/description
  One ICE agent (libjuice), one DTLS handshake (OpenSSL, RFC 5764) and one
  SRTP context (libuvsrtp) per media stream, behind a small API: create, the
  remote credentials, trickled candidates, send, and callbacks for the
  received media and the transport events. OpenSSL must be built with DTLS
  (CONFIG_OPENSSL_WITH_DTLS).
endef

define Build/InstallDev
	$(INSTALL_DIR) $(1)/usr/include/uvtransport $(1)/usr/lib/pkgconfig
	$(CP) $(PKG_BUILD_DIR)/src/include/uvtransport/uvtransport.h \
		$(1)/usr/include/uvtransport/
	$(CP) $(PKG_INSTALL_DIR)/usr/lib/libuvtransport.so* $(1)/usr/lib/
	$(CP) $(PKG_INSTALL_DIR)/usr/lib/pkgconfig/uvtransport.pc \
		$(1)/usr/lib/pkgconfig/
endef

define Package/libuvtransport/install
	$(INSTALL_DIR) $(1)/usr/lib
	$(CP) $(PKG_INSTALL_DIR)/usr/lib/libuvtransport.so.* $(1)/usr/lib/
endef

$(eval $(call BuildPackage,libuvtransport))
