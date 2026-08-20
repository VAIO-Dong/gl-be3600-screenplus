'use strict';
'require view';
'require form';

function addPage(map, name, title, fields) {
	var section = map.section(form.NamedSection, name, 'page', title);
	section.anonymous = true;
	section.addremove = false;

	var option = section.option(form.Flag, 'enabled', _('Show this page'));
	option.default = option.enabled;

	option = section.option(form.MultiValue, 'field', _('Visible fields'));
	option.widget = 'checkbox';
	option.rmempty = false;
	fields.forEach(function(field) {
		option.value(field[0], field[1]);
	});
}

function addPageOrder(map) {
	var section = map.section(form.NamedSection, 'page_order', 'page_order', _('Page order'));
	section.anonymous = true;
	section.addremove = false;
	section.description = _('Lower numbers appear first. Page visibility and content are configured below.');

	[
		[ 'home', _('Home / clock'), '10' ],
		[ 'status', _('Device status'), '20' ],
		[ 'traffic', _('Network traffic'), '25' ],
		[ 'network', _('Network'), '30' ],
		[ 'wifi', _('Wi-Fi credentials'), '40' ],
		[ 'openclash', _('OpenClash'), '50' ]
	].forEach(function(page) {
		var option = section.option(form.Value, page[0], page[1]);
		option.datatype = 'range(0,999)';
		option.default = page[2];
		option.rmempty = false;
	});
}

return view.extend({
	render: function() {
		var map = new form.Map('screenplus', _('Screen pages'),
			_('Set the swipe order first, then enable pages and choose the data shown on each page.'));

		addPageOrder(map);

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
