'use strict';
'require view';
'require form';

return view.extend({
	render: function() {
		var m = new form.Map('screenplus', _('ScreenPlus'),
			_('Choose how the screen looks, responds to touch and protects your Wi-Fi password.'));
		var s = m.section(form.NamedSection, 'main', 'screenplus', _('Display and controls'));
		s.anonymous = true;
		s.addremove = false;

		var o = s.option(form.Flag, 'enabled', _('Use ScreenPlus'));
		o.default = o.enabled;
		o.description = _('Turn this off to use the original GL.iNet screen.');

		o = s.option(form.ListValue, 'language', _('Screen language'));
		o.value('zh_cn', _('简体中文'));
		o.value('en', _('English'));
		o.default = 'zh_cn';

		o = s.option(form.Value, 'brightness', _('Screen brightness'));
		o.datatype = 'range(1,11)';
		o.default = '5';
		o.rmempty = true;

		o = s.option(form.ListValue, 'rotation', _('Screen orientation'));
		o.value('90', _('Standard'));
		o.value('270', _('Rotate 180°'));
		o.default = '90';

		o = s.option(form.Flag, 'always_on', _('Keep the screen on'));
		o.default = o.disabled;

		o = s.option(form.Value, 'idle_timeout', _('Turn off after (seconds)'));
		o.datatype = 'range(10,86400)';
		o.default = '180';
		o.rmempty = true;
		o.depends('always_on', '0');
		o.description = _('The screen turns off after this much time without a touch.');

		o = s.option(form.Flag, 'swipe_loop', _('Loop through pages'));
		o.default = o.enabled;
		o.description = _('Swiping past the last page returns to the first page.');

		o = s.option(form.ListValue, 'page_transition', _('Swipe animation'));
		o.value('slide', _('Smooth (recommended)'));
		o.value('none', _('Instant'));
		o.default = 'slide';
		o.description = _('Choose how the page settles into place after you lift your finger.');

		o = s.option(form.Flag, 'auto_carousel', _('Change pages automatically'));
		o.default = o.disabled;

		o = s.option(form.Value, 'carousel_interval', _('Change page every (seconds)'));
		o.datatype = 'range(3,300)';
		o.default = '10';
		o.rmempty = true;
		o.depends('auto_carousel', '1');

		o = s.option(form.ListValue, 'password_mode', _('Wi-Fi password privacy'));
		o.value('hidden', _('Hide password and QR code'));
		o.value('tap', _('Tap to show password'));
		o.value('visible', _('Always show password'));
		o.value('qr', _('Show QR code prompt'));
		o.default = 'tap';
		o.description = _('Press and hold a Wi-Fi row to open its sharing QR code, unless password sharing is hidden.');

		return m.render();
	}
});
