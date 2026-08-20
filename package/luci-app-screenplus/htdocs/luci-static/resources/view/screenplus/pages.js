'use strict';
'require view';
'require form';

function addPage(map, name, title, fields) {
	var section = map.section(form.NamedSection, name, 'page', title);
	section.anonymous = true;
	section.addremove = false;

	var option = section.option(form.Flag, 'enabled', _('Show this page'));
	option.default = option.enabled;

	option = section.option(form.Value, 'order', _('Page order'));
	option.datatype = 'range(0,999)';
	option.rmempty = false;

	option = section.option(form.ListValue, 'background', _('Background image'));
	option.value('', _('Use theme colour'));
	option.value(name + '.rgb565', _('Use uploaded image'));
	option.default = '';
	option.description = _('Upload or replace this image on the Appearance tab.');

	option = section.option(form.MultiValue, 'field', _('Visible fields'));
	option.widget = 'checkbox';
	option.rmempty = false;
	fields.forEach(function(field) {
		option.value(field[0], field[1]);
	});
}

return view.extend({
	render: function() {
		var map = new form.Map('screenplus', _('Screen pages'),
			_('Enable pages, change their swipe order, and choose the data shown on each page.'));

		addPage(map, 'home', _('Home / clock'), [
			[ 'time', _('Time') ],
			[ 'seconds', _('Seconds') ],
			[ 'date', _('Date') ],
			[ 'weekday', _('Weekday') ],
			[ 'timezone', _('Time zone') ]
		]);
		addPage(map, 'status', _('Device status'), [
			[ 'cpu', _('CPU utilisation and temperature') ],
			[ 'memory', _('Memory utilisation and used space') ],
			[ 'fan', _('Fan speed') ]
		]);
		addPage(map, 'traffic', _('Network traffic'), [
			[ 'rates', _('Live upload and download rates') ],
			[ 'history', _('30-second traffic history') ]
		]);
		addPage(map, 'network', _('Network'), [
			[ 'lan', _('LAN IP address') ],
			[ 'ethernet', _('Ethernet WAN') ],
			[ 'repeater', _('Wi-Fi repeater WAN') ],
			[ 'tethering', _('USB tethering WAN') ],
			[ 'cellular', _('Cellular WAN') ],
			[ 'wan_detail', _('Active WAN address') ]
		]);
		addPage(map, 'wifi', _('Wi-Fi credentials'), [
			[ 'wifi_2g', _('2.4 GHz SSID and password') ],
			[ 'wifi_5g', _('5 GHz SSID and password') ]
		]);
		addPage(map, 'openclash', _('OpenClash'), [
			[ 'rates', _('Current upload and download rates') ],
			[ 'connections', _('Current connection count') ],
			[ 'totals', _('Total upload and download traffic') ]
		]);

		return map.render();
	}
});
