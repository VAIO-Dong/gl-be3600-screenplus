include $(TOPDIR)/rules.mk

PKG_NAME:=screenplus
PKG_VERSION:=1.1.3
PKG_RELEASE:=1
PKG_LICENSE:=MIT OFL-1.1
PKG_SOURCE_PROTO:=git
PKG_SOURCE_URL:=https://github.com/lvgl/lvgl.git
PKG_SOURCE_VERSION:=85aa60d18b3d5e5588d7b247abf90198f07c8a63
PKG_MIRROR_HASH:=skip

include $(INCLUDE_DIR)/package.mk

define Package/screenplus
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=Touchscreen dashboard for GL-BE3600
  DEPENDS:=+libc +luci-base
  PROVIDES:=luci-app-screenplus
  CONFLICTS:=luci-app-screenplus
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
	$(INSTALL_BIN) ./files/screenplus-run $(1)/usr/libexec/screenplus-run
	$(INSTALL_BIN) ./files/screenplus-reset-threshold $(1)/usr/libexec/screenplus-reset-threshold
	$(INSTALL_BIN) ./files/screenplus-toggle-ui $(1)/usr/libexec/screenplus-toggle-ui
	$(INSTALL_BIN) ./files/screenplus-toggle-ui.lua $(1)/usr/libexec/screenplus-toggle-ui.lua
	$(INSTALL_DIR) $(1)/usr/share/screenplus/backgrounds
	$(INSTALL_DIR) $(1)/usr/share/licenses/screenplus
	$(INSTALL_DATA) $(CURDIR)/../../docs/fonts/OFL-NotoSansSC.txt \
		$(1)/usr/share/licenses/screenplus/OFL-NotoSansSC.txt
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./files/screenplus.init $(1)/etc/init.d/screenplus
	$(INSTALL_DIR) $(1)/etc/config
	$(INSTALL_CONF) ./files/screenplus.config $(1)/etc/config/screenplus
	$(INSTALL_DIR) $(1)/etc/gl-switch.d
	$(INSTALL_BIN) ./files/ScreenPlus.sh $(1)/etc/gl-switch.d/screenplus.sh
	$(INSTALL_DIR) $(1)/etc/hotplug.d/button
	$(INSTALL_BIN) ./files/99-screenplus-reset $(1)/etc/hotplug.d/button/99-screenplus-reset
	$(INSTALL_DIR) $(1)/usr/share/luci/menu.d
	$(INSTALL_DATA) ../luci-app-screenplus/root/usr/share/luci/menu.d/luci-app-screenplus.json \
		$(1)/usr/share/luci/menu.d/luci-app-screenplus.json
	$(INSTALL_DIR) $(1)/usr/share/rpcd/acl.d
	$(INSTALL_DATA) ../luci-app-screenplus/root/usr/share/rpcd/acl.d/luci-app-screenplus.json \
		$(1)/usr/share/rpcd/acl.d/luci-app-screenplus.json
	$(INSTALL_DIR) $(1)/www/luci-static/resources/view/screenplus
	$(INSTALL_DATA) ../luci-app-screenplus/htdocs/luci-static/resources/view/screenplus/*.js \
		$(1)/www/luci-static/resources/view/screenplus/
endef

define Package/screenplus/postinst
#!/bin/sh
[ -n "$${IPKG_INSTROOT}" ] && exit 0
/etc/init.d/gl_screen stop 2>/dev/null
/etc/init.d/gl_screen disable 2>/dev/null
/usr/libexec/screenplus-migrate 2>/dev/null
/usr/libexec/screenplus-reset-threshold restore 2>/dev/null || \
	logger -t screenplus "legacy reset threshold change was not restored"
rm -f /etc/gl-switch.d/ScreenPlus.sh
if [ "$$(uci -q get switch-button.@main[0].func)" = "ScreenPlus" ]; then
	uci set switch-button.@main[0].func=screenplus
	uci commit switch-button
fi
/usr/libexec/screenplus-toggle-ui apply 2>/dev/null || \
	logger -t screenplus "native Toggle frontend patch was not applied"
/etc/init.d/screenplus enable
/etc/init.d/screenplus start
rm -f /tmp/luci-indexcache* /tmp/luci-modulecache/* 2>/dev/null
/etc/init.d/rpcd restart 2>/dev/null
if [ "$$(uci -q get switch-button.@main[0].func)" = "screenplus" ] && \
   [ -x /etc/init.d/gl_switch_button_check ]; then
	/etc/init.d/gl_switch_button_check start 2>/dev/null
fi
exit 0
endef

define Package/screenplus/prerm
#!/bin/sh
[ -n "$${IPKG_INSTROOT}" ] && exit 0
/etc/init.d/screenplus stop 2>/dev/null
/etc/init.d/screenplus disable 2>/dev/null
rm -f /tmp/screenplus-reset-button
/usr/libexec/screenplus-reset-threshold restore 2>/dev/null || true
if [ "$${PKG_UPGRADE}" != "1" ]; then
	/usr/libexec/screenplus-toggle-ui restore 2>/dev/null || true
	if [ "$$(uci -q get switch-button.@main[0].func)" = "screenplus" ]; then
		uci -q delete switch-button.@main[0].func
		uci commit switch-button
	fi
fi
exit 0
endef

define Package/screenplus/postrm
#!/bin/sh
[ -n "$${IPKG_INSTROOT}" ] && exit 0
[ "$${PKG_UPGRADE}" = "1" ] && exit 0
if [ -x /etc/init.d/gl_screen ]; then
	/etc/init.d/gl_screen enable
	/etc/init.d/gl_screen start
fi
rm -f /tmp/luci-indexcache* /tmp/luci-modulecache/* 2>/dev/null
/etc/init.d/rpcd restart 2>/dev/null
exit 0
endef

$(eval $(call BuildPackage,screenplus))
