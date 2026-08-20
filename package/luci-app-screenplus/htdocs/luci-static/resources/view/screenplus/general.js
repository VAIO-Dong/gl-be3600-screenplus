'use strict';
'require view';
'require form';

return view.extend({
	render: function() {
		var m = new form.Map('screenplus', _('ScreenPlus'),
			_('Direct touchscreen dashboard for the GL-BE3600.'));
		var s = m.section(form.NamedSection, 'main', 'screenplus', _('General'));
		s.anonymous = true;
		s.addremove = false;

		var o = s.option(form.Flag, 'enabled', _('Enable ScreenPlus'));
		o.default = o.enabled;

		o = s.option(form.ListValue, 'language', _('Interface language'));
		o.value('zh_cn', _('Simplified Chinese'));
		o.value('en', _('English'));
		o.default = 'zh_cn';

		o = s.option(form.Value, 'brightness', _('Brightness'));
		o.datatype = 'range(1,11)';
		o.default = '5';

		o = s.option(form.ListValue, 'rotation', _('Screen orientation'));
		o.value('90', _('Normal'));
		o.value('270', _('Flipped'));
		o.default = '90';

		o = s.option(form.Flag, 'always_on', _('Always keep display on'));
		o.default = o.disabled;

		o = s.option(form.Value, 'idle_timeout', _('Idle timeout'));
		o.datatype = 'range(10,86400)';
		o.default = '180';
		o.depends('always_on', '0');
		o.description = _('Seconds before the backlight turns off.');

		o = s.option(form.Flag, 'swipe_loop', _('Loop when swiping pages'));
		o.default = o.enabled;

		o = s.option(form.ListValue, 'page_transition', _('Page transition'));
		o.value('slide', _('Follow touch with smooth settle (recommended)'));
		o.value('none', _('Follow touch with instant settle'));
		o.default = 'slide';
		o.description = _('Pages follow the finger directly; this controls the short settling motion after release.');

		o = s.option(form.Flag, 'auto_carousel', _('Automatic page carousel'));
		o.default = o.disabled;

		o = s.option(form.Value, 'carousel_interval', _('Carousel interval'));
		o.datatype = 'range(3,300)';
		o.default = '10';
		o.depends('auto_carousel', '1');
		o.description = _('Seconds between automatic page changes.');

		o = s.option(form.ListValue, 'password_mode', _('Wi-Fi password display'));
		o.value('hidden', _('Always hidden'));
		o.value('tap', _('Tap to reveal'));
		o.value('visible', _('Always visible'));
		o.value('qr', _('QR code'));
		o.default = 'tap';

		o = s.option(form.Flag, 'restore_official_on_remove',
			_('Restore official screen service when uninstalling'));
		o.default = o.enabled;

		return m.render();
	}
});
