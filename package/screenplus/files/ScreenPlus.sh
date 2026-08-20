#!/bin/sh

# GL.iNet switch-button extension. The filename is the option shown by the
# native Toggle settings page and the first argument is the physical position.
case "$1" in
	on)
		uci -q set screenplus.main.enabled=1
		uci -q commit screenplus
		/etc/init.d/gl_screen stop 2>/dev/null
		/etc/init.d/gl_screen disable 2>/dev/null
		/etc/init.d/screenplus enable 2>/dev/null
		/etc/init.d/screenplus restart 2>/dev/null
		logger -t screenplus "side switch enabled ScreenPlus"
		;;
	off)
		uci -q set screenplus.main.enabled=0
		uci -q commit screenplus
		/etc/init.d/screenplus stop 2>/dev/null
		if [ "$(uci -q get screenplus.main.restore_official_on_remove)" != "0" ] &&
		   [ -x /etc/init.d/gl_screen ]; then
			/etc/init.d/gl_screen enable 2>/dev/null
			/etc/init.d/gl_screen start 2>/dev/null
		fi
		logger -t screenplus "side switch disabled ScreenPlus"
		;;
esac

exit 0
