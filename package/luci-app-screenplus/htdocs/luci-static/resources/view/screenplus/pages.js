'use strict';
'require view';
'require form';
'require uci';

var PAGE_DEFAULTS = [
	[ 'home', _('Home / clock'), '10' ],
	[ 'traffic', _('Network traffic'), '20' ],
	[ 'status', _('Device status'), '30' ],
	[ 'wifi', _('Wi-Fi credentials'), '40' ],
	[ 'network', _('LAN / WAN connections'), '50' ],
	[ 'openclash', _('OpenClash'), '60' ]
];

function getFields(sectionId) {
	var values = uci.get('screenplus', sectionId, 'field');
	if (!Array.isArray(values))
		values = values ? [ values ] : [];
	return values.filter(function(value) { return value !== 'none'; });
}

function setMappedFields(sectionId, names, enabled) {
	var values = getFields(sectionId);
	names.forEach(function(name) {
		var index = values.indexOf(name);
		if (enabled && index < 0)
			values.push(name);
		else if (!enabled && index >= 0)
			values.splice(index, 1);
	});
	/* Keep an explicit list entry when every field is disabled. Without it,
	 * the native service correctly interprets a missing list as defaults. */
	uci.set('screenplus', sectionId, 'field', values.length ? values : [ 'none' ]);
}

function addFieldFlag(section, sectionId, spec) {
	var names = Array.isArray(spec[2]) ? spec[2] : [ spec[2] || spec[0] ];
	var option = section.option(form.Flag, '_field_' + spec[0], spec[1]);
	option.default = option.enabled;
	option.rmempty = false;
	if (spec[3])
		option.description = spec[3];
	option.cfgvalue = function() {
		var values = getFields(sectionId);
		return names.some(function(name) { return values.indexOf(name) >= 0; }) ?
			this.enabled : this.disabled;
	};
	option.write = function(sectionName, value) {
		setMappedFields(sectionName, names, value === this.enabled);
	};
	option.remove = function(sectionName) {
		setMappedFields(sectionName, names, false);
	};
}

function addPage(map, name, title, description, fields) {
	var section = map.section(form.NamedSection, name, 'page', title);
	section.anonymous = true;
	section.addremove = false;
	section.description = description;

	var option = section.option(form.Flag, 'enabled', _('Show this page'));
	option.default = option.enabled;

	fields.forEach(function(field) {
		addFieldFlag(section, name, field);
	});
}

function addPageOrder(map) {
	var section = map.section(form.NamedSection, 'page_order', 'page_order', _('Page order'));
	section.anonymous = true;
	section.addremove = false;
	section.description = _('Lower numbers appear first. The six built-in values are real, savable settings, so this form never opens empty. Clearing a value restores its default.');

	PAGE_DEFAULTS.forEach(function(page) {
		var option = section.option(form.Value, page[0], page[1]);
		option.datatype = 'range(0,999)';
		option.default = page[2];
		option.placeholder = page[2];
		option.rmempty = true;
		option.cfgvalue = function(sectionId) {
			return uci.get('screenplus', sectionId, page[0]) || page[2];
		};
		option.write = function(sectionId, value) {
			uci.set('screenplus', sectionId, page[0], value || page[2]);
		};
		option.remove = function(sectionId) {
			uci.set('screenplus', sectionId, page[0], page[2]);
		};
	});
}

return view.extend({
	load: function() {
		return uci.load('screenplus').then(function() {
			if (!uci.get('screenplus', 'page_order'))
				uci.add('screenplus', 'page_order', 'page_order');
		});
	},

	render: function() {
		var map = new form.Map('screenplus', _('Screen pages'),
			_('The configuration follows the physical screen from the first swipe page to the last, and each page follows its top-to-bottom visual layout.'));

		addPageOrder(map);

		addPage(map, 'home', _('Home / clock'),
			_('Clock content arranged beside the home-page accent line.'), [
			[ 'time', _('Time') ],
			[ 'seconds', _('Seconds') ],
			[ 'date', _('Date') ],
			[ 'weekday', _('Weekday') ],
			[ 'timezone', _('Time zone') ]
		]);
		addPage(map, 'traffic', _('Network traffic'),
			_('Left column: upload, download and connection count. Right column: the 30-second graph.'), [
			[ 'rates', _('Live upload and download rates') ],
			[ 'connections', _('Current connection count') ],
			[ 'history', _('30-second traffic history') ]
		]);
		addPage(map, 'status', _('Device status'),
			_('Three equal rows from top to bottom: CPU, memory and fan.'), [
			[ 'cpu', _('CPU utilisation and temperature') ],
			[ 'memory', _('Memory utilisation and used space') ],
			[ 'fan', _('Fan speed') ]
		]);
		addPage(map, 'wifi', _('Wi-Fi credentials'),
			_('Two equal rows matching the screen: 2.4 GHz first, then 5 GHz.'), [
			[ 'wifi_2g', _('2.4 GHz SSID and password') ],
			[ 'wifi_5g', _('5 GHz SSID and password') ]
		]);
		addPage(map, 'network', _('LAN / WAN connections'),
			_('Top row: four WAN methods. Middle row: active WAN. Bottom row: shared LAN address.'), [
			[ 'ethernet', _('Ethernet WAN') ],
			[ 'repeater', _('Wi-Fi repeater WAN') ],
			[ 'tethering', _('USB tethering WAN') ],
			[ 'cellular', _('Cellular WAN') ],
			[ 'wan_detail', _('Active WAN type and address') ],
			[ 'lan', _('LAN address') ]
		]);
		addPage(map, 'openclash', _('OpenClash'),
			_('Top row: service state and switch. Middle row: rate with total traffic. Bottom row: connections and resources.'), [
			[ 'traffic', _('Upload/download rates and total traffic'), [ 'rates', 'totals' ] ],
			[ 'connections', _('Current connection count') ],
			[ 'resources', _('CPU and memory usage') ]
		]);

		return map.render();
	}
});
