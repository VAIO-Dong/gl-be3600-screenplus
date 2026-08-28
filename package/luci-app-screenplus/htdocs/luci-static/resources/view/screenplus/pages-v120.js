'use strict';
'require view';
'require form';
'require uci';

var PAGE_DEFAULTS = [
	[ 'home', _('Home'), '10' ],
	[ 'traffic', _('Live traffic'), '20' ],
	[ 'status', _('System status'), '30' ],
	[ 'wifi', _('Wi-Fi'), '40' ],
	[ 'network', _('Network connections'), '50' ],
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

function addFieldFlag(section, sectionId, spec, tabName) {
	var names = Array.isArray(spec[2]) ? spec[2] : [ spec[2] || spec[0] ];
	var option = section.taboption(tabName, form.Flag,
		'_page_' + sectionId + '_field_' + spec[0], spec[1]);
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
		setMappedFields(sectionId, names, value === this.enabled);
	};
	option.remove = function(sectionName) {
		setMappedFields(sectionId, names, false);
	};
}

function addPageTab(section, name, title, description, fields) {
	section.tab(name, title, description);
	var option = section.taboption(name, form.Flag,
		'_page_' + name + '_enabled', _('Display this page'));
	option.default = option.enabled;
	option.rmempty = false;
	option.cfgvalue = function() {
		var value = uci.get('screenplus', name, 'enabled');
		return value == null ? this.enabled : value;
	};
	option.write = function(sectionName, value) {
		uci.set('screenplus', name, 'enabled', value);
	};
	option.remove = function(sectionName) {
		uci.set('screenplus', name, 'enabled', this.disabled);
	};

	fields.forEach(function(field) {
		addFieldFlag(section, name, field, name);
	});
}

function addPageOrder(map) {
	var section = map.section(form.NamedSection, 'page_order', 'page_order', _('Page order'));
	section.anonymous = true;
	section.addremove = false;
	section.description = _('Pages with smaller numbers appear first. Clear a value to restore its original position.');

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
		var map = new form.Map('screenplus', _('Pages and content'),
			_('Arrange the swipe order, then open a page tab to choose what appears on screen. The layout adapts automatically to your choices.'));

		addPageOrder(map);
		var pages = map.section(form.NamedSection, 'home', 'page', _('Page content'));
		pages.anonymous = true;
		pages.addremove = false;

		addPageTab(pages, 'home', _('Home'),
			_('A clear clock for quick checks at a glance.'), [
			[ 'time', _('Time') ],
			[ 'seconds', _('Seconds') ],
			[ 'date', _('Date') ],
			[ 'weekday', _('Weekday') ],
			[ 'timezone', _('Time zone name') ]
		]);
		addPageTab(pages, 'traffic', _('Live traffic'),
			_('See current network activity and recent traffic changes.'), [
			[ 'rates', _('Upload and download speed') ],
			[ 'connections', _('Active connections') ],
			[ 'history', _('30-second traffic chart') ]
		]);
		addPageTab(pages, 'status', _('System status'),
			_('Keep an eye on the router’s workload and cooling.'), [
			[ 'cpu', _('CPU usage and temperature') ],
			[ 'memory', _('Memory usage') ],
			[ 'fan', _('Fan speed') ]
		]);
		addPageTab(pages, 'wifi', _('Wi-Fi'),
			_('Quickly check or share your Wi-Fi name and password.'), [
			[ 'wifi_2g', _('2.4 GHz Wi-Fi name and password') ],
			[ 'wifi_5g', _('5 GHz Wi-Fi name and password') ]
		]);
		addPageTab(pages, 'network', _('Network connections'),
			_('Check how the router reaches the internet and view its internet and local network addresses.'), [
			[ 'ethernet', _('Ethernet') ],
			[ 'repeater', _('Wi-Fi repeater') ],
			[ 'tethering', _('USB tethering') ],
			[ 'cellular', _('Cellular modem') ],
			[ 'wan_detail', _('Current internet connection') ],
			[ 'lan', _('Local network IP address') ]
		]);
		addPageTab(pages, 'openclash', _('OpenClash'),
			_('View OpenClash activity and control it directly from the screen.'), [
			[ 'traffic', _('Speed and total data'), [ 'rates', 'totals' ] ],
			[ 'connections', _('Active connections') ],
			[ 'resources', _('CPU and memory usage') ]
		]);

		return map.render();
	}
});
