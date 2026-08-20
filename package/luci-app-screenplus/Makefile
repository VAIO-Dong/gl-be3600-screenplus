include $(TOPDIR)/rules.mk

LUCI_TITLE:=LuCI support for ScreenPlus
LUCI_DEPENDS:=+screenplus +luci-base
LUCI_PKGARCH:=all
PKG_VERSION:=0.1.0
PKG_RELEASE:=1

define Package/luci-app-screenplus/postinst
#!/bin/sh
[ -n "$${IPKG_INSTROOT}" ] && exit 0
rm -f /tmp/luci-indexcache /tmp/luci-modulecache/* 2>/dev/null
/etc/init.d/rpcd restart 2>/dev/null
exit 0
endef

define Package/luci-app-screenplus/postrm
#!/bin/sh
[ -n "$${IPKG_INSTROOT}" ] && exit 0
rm -f /tmp/luci-indexcache /tmp/luci-modulecache/* 2>/dev/null
/etc/init.d/rpcd restart 2>/dev/null
exit 0
endef

include $(TOPDIR)/feeds/luci/luci.mk
