include $(TOPDIR)/rules.mk

PKG_NAME:=screenplus
PKG_VERSION:=0.1.0
PKG_RELEASE:=1
PKG_LICENSE:=MIT
PKG_SOURCE_PROTO:=git
PKG_SOURCE_URL:=https://github.com/lvgl/lvgl.git
PKG_SOURCE_VERSION:=85aa60d18b3d5e5588d7b247abf90198f07c8a63
PKG_MIRROR_HASH:=skip

include $(INCLUDE_DIR)/package.mk

define Package/screenplus
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=Touchscreen dashboard for GL-BE3600
  DEPENDS:=+libc
endef

define Package/screenplus/description
 Native framebuffer and touchscreen dashboard replacing GL.iNet gl_screen.
endef

define Package/screenplus/conffiles
/etc/config/screenplus
endef

define Build/Prepare
	$(call Build/Prepare/Default)
	$(CP) $(CURDIR)/../../src $(PKG_BUILD_DIR)/screenplus-src
endef

define Build/Compile
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_CPPFLAGS) \
		-O2 -std=c11 -pthread -ffunction-sections -fdata-sections \
		-D_GNU_SOURCE -DLV_CONF_INCLUDE_SIMPLE \
		-I$(PKG_BUILD_DIR)/screenplus-src -I$(PKG_BUILD_DIR) \
		-Wno-unused-function -Wno-unused-variable -Wno-unused-parameter \
		-o $(PKG_BUILD_DIR)/screenplus \
		$(PKG_BUILD_DIR)/screenplus-src/*.c \
		$$(find $(PKG_BUILD_DIR)/src -type f -name '*.c' ! -path '*/debugging/test/*') \
		$(TARGET_LDFLAGS) -Wl,--gc-sections -lm
endef

define Package/screenplus/install
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/screenplus $(1)/usr/sbin/screenplus
	$(INSTALL_DIR) $(1)/usr/libexec
	$(INSTALL_BIN) ./files/screenplus-background $(1)/usr/libexec/screenplus-background
	$(INSTALL_BIN) ./files/screenplus-diagnostics $(1)/usr/libexec/screenplus-diagnostics
	$(INSTALL_BIN) ./files/screenplus-migrate $(1)/usr/libexec/screenplus-migrate
	$(INSTALL_DIR) $(1)/usr/share/screenplus/backgrounds
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./files/screenplus.init $(1)/etc/init.d/screenplus
	$(INSTALL_DIR) $(1)/etc/config
	$(INSTALL_CONF) ./files/screenplus.config $(1)/etc/config/screenplus
endef

define Package/screenplus/postinst
#!/bin/sh
[ -n "$${IPKG_INSTROOT}" ] && exit 0
/etc/init.d/gl_screen stop 2>/dev/null
/etc/init.d/gl_screen disable 2>/dev/null
/usr/libexec/screenplus-migrate 2>/dev/null
/etc/init.d/screenplus enable
/etc/init.d/screenplus start
exit 0
endef

define Package/screenplus/prerm
#!/bin/sh
[ -n "$${IPKG_INSTROOT}" ] && exit 0
/etc/init.d/screenplus stop 2>/dev/null
/etc/init.d/screenplus disable 2>/dev/null
exit 0
endef

define Package/screenplus/postrm
#!/bin/sh
[ -n "$${IPKG_INSTROOT}" ] && exit 0
[ "$${PKG_UPGRADE}" = "1" ] && exit 0
restore="$$(uci -q get screenplus.main.restore_official_on_remove)"
[ -n "$${restore}" ] || restore=1
if [ "$${restore}" = "1" ] && [ -x /etc/init.d/gl_screen ]; then
	/etc/init.d/gl_screen enable
	/etc/init.d/gl_screen start
fi
exit 0
endef

$(eval $(call BuildPackage,screenplus))
