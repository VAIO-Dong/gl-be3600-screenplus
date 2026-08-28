'use strict';
'require view';
'require form';

var WEEKDAYS = [
	[ 'monday', _('Monday') ],
	[ 'tuesday', _('Tuesday') ],
	[ 'wednesday', _('Wednesday') ],
	[ 'thursday', _('Thursday') ],
	[ 'friday', _('Friday') ],
	[ 'saturday', _('Saturday') ],
	[ 'sunday', _('Sunday') ]
];

function validTime(value) {
	var match = /^(\d{2}):(\d{2})$/.exec(value || '');
	return !!(match && +match[1] < 24 && +match[2] < 60);
}

var ScheduleRangeValue = form.Value.extend({
	cfgvalue: function(sectionId) {
		var config = this.uciconfig || this.section.uciconfig || this.map.config;
		var enabled = this.rangeCanDisable ?
			this.map.data.get(config, sectionId, this.rangePrefix + '_enabled') : '1';
		return {
			enabled: enabled == null ? this.rangeDefaultEnabled : enabled,
			on: this.map.data.get(config, sectionId, this.rangePrefix + '_on') ||
				this.rangeDefaultOn,
			off: this.map.data.get(config, sectionId, this.rangePrefix + '_off') ||
				this.rangeDefaultOff
		};
	},

	renderWidget: function(sectionId, optionIndex, value) {
		var enabled = !this.rangeCanDisable || value.enabled !== '0';
		var on = E('input', {
			'type': 'time',
			'step': '60',
			'inputmode': 'numeric',
			'class': 'cbi-input-text',
			'data-role': 'on',
			'style': 'width:8.5em;min-width:8.5em',
			'value': value.on
		});
		var off = E('input', {
			'type': 'time',
			'step': '60',
			'inputmode': 'numeric',
			'class': 'cbi-input-text',
			'data-role': 'off',
			'style': 'width:8.5em;min-width:8.5em',
			'value': value.off
		});
		var checkbox = this.rangeCanDisable ? E('input', {
			'type': 'checkbox',
			'class': 'cbi-input-checkbox',
			'data-role': 'enabled',
			'checked': enabled ? '' : null
		}) : null;
		var children = [];
		if (checkbox) {
			children.push(E('label', {
				'style': 'display:inline-flex;align-items:center;gap:.35em;min-width:5.5em'
			}, [ checkbox, E('span', {}, [ _('Active') ]) ]));
		}
		children.push(on, E('span', {}, [ _('to') ]), off);
		var node = E('div', {
			'id': this.cbid(sectionId),
			'class': 'screenplus-schedule-range',
			'style': 'display:flex;align-items:center;gap:.65em;flex-wrap:wrap'
		}, children);
		var setEnabledState = function() {
			var active = !checkbox || checkbox.checked;
			on.disabled = !active;
			off.disabled = !active;
			on.style.opacity = active ? '' : '.45';
			off.style.opacity = active ? '' : '.45';
		};
		var notifyChange = function() {
			node.dispatchEvent(new Event('widget-change', { bubbles: true }));
		};
		on.addEventListener('change', notifyChange);
		off.addEventListener('change', notifyChange);
		if (checkbox) {
			checkbox.addEventListener('change', function() {
				setEnabledState();
				notifyChange();
			});
		}
		setEnabledState();
		return node;
	},

	getRange: function(sectionId) {
		var node = document.getElementById(this.cbid(sectionId));
		if (!node)
			return null;
		var enabled = node.querySelector('[data-role="enabled"]');
		return {
			enabled: enabled ? enabled.checked : true,
			on: node.querySelector('[data-role="on"]').value,
			off: node.querySelector('[data-role="off"]').value
		};
	},

	setRange: function(sectionId, value) {
		var node = document.getElementById(this.cbid(sectionId));
		if (!node)
			return;
		var enabled = node.querySelector('[data-role="enabled"]');
		if (enabled) {
			enabled.checked = value.enabled;
			enabled.dispatchEvent(new Event('change', { bubbles: true }));
		}
		var on = node.querySelector('[data-role="on"]');
		var off = node.querySelector('[data-role="off"]');
		on.value = value.on;
		off.value = value.off;
		on.dispatchEvent(new Event('change', { bubbles: true }));
		off.dispatchEvent(new Event('change', { bubbles: true }));
	},

	parse: function(sectionId) {
		if (!this.isActive(sectionId))
			return Promise.resolve();
		var value = this.getRange(sectionId);
		if (!value || !validTime(value.on) || !validTime(value.off)) {
			return Promise.reject(new TypeError(
				_('Choose both an on and off time for %s.').format(this.title)));
		}
		var config = this.uciconfig || this.section.uciconfig || this.map.config;
		if (this.rangeCanDisable) {
			this.map.data.set(config, sectionId, this.rangePrefix + '_enabled',
				value.enabled ? '1' : '0');
		}
		this.map.data.set(config, sectionId, this.rangePrefix + '_on', value.on);
		this.map.data.set(config, sectionId, this.rangePrefix + '_off', value.off);
		return Promise.resolve();
	}
});

return view.extend({
	addScheduleRange: function(section, optionName, title, prefix, canDisable,
		defaultEnabled, defaultOn, defaultOff, dependency) {
		var option = section.option(ScheduleRangeValue, optionName, title);
		option.rangePrefix = prefix;
		option.rangeCanDisable = canDisable;
		option.rangeDefaultEnabled = defaultEnabled ? '1' : '0';
		option.rangeDefaultOn = defaultOn;
		option.rangeDefaultOff = defaultOff;
		option.depends(dependency);
		return option;
	},

	copyScheduleDay: function(source, status) {
		var value = this.customScheduleFields[source].getRange('main');
		if (!value)
			return;
		WEEKDAYS.forEach(L.bind(function(day) {
			if (day[0] !== source)
				this.customScheduleFields[day[0]].setRange('main', value);
		}, this));
		status.textContent = _('Copied. Save or Apply to keep these settings.');
		window.setTimeout(function() {
			if (status.textContent)
				status.textContent = '';
		}, 3000);
	},

	renderScheduleCopy: function() {
		var source = E('select', { 'class': 'cbi-input-select' },
			WEEKDAYS.map(function(day) {
				return E('option', { 'value': day[0] }, [ day[1] ]);
			}));
		var status = E('span', { 'style': 'margin-left:.75em' });
		var button = E('button', {
			'type': 'button',
			'class': 'btn cbi-button cbi-button-action',
			'click': L.bind(function() {
				this.copyScheduleDay(source.value, status);
			}, this)
		}, [ _('Copy to all other days') ]);
		return E('div', { 'style': 'display:flex;align-items:center;gap:.5em;flex-wrap:wrap' },
			[ source, button, status ]);
	},

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
		o.description = _('Keep the screen continuously on, or use the schedule below. Turn this off to use the inactivity timeout instead.');

		o = s.option(form.Flag, 'schedule_enabled', _('Use screen schedule'));
		o.default = o.disabled;
		o.depends('always_on', '1');
		o.description = _('The screen stays on only during the selected time ranges. The schedule repeats every week.');

		o = s.option(form.ListValue, 'schedule_mode', _('Schedule type'));
		o.value('daily', _('Every day'));
		o.value('workweek', _('Weekdays and weekends'));
		o.value('weekly', _('Custom days'));
		o.default = 'daily';
		o.depends({ always_on: '1', schedule_enabled: '1' });
		o.description = _('Turn off a row to keep the screen off for those days. An end time earlier than the start continues past midnight; equal times mean all day.');

		this.addScheduleRange(s, '_schedule_daily_range', _('Every day'),
			'schedule_daily', false, true, '08:00', '23:00',
			{ always_on: '1', schedule_enabled: '1', schedule_mode: 'daily' });

		this.addScheduleRange(s, '_schedule_weekday_range', _('Monday to Friday'),
			'schedule_weekday', true, true, '09:00', '19:00',
			{ always_on: '1', schedule_enabled: '1', schedule_mode: 'workweek' });
		this.addScheduleRange(s, '_schedule_weekend_range', _('Saturday and Sunday'),
			'schedule_weekend', true, false, '09:00', '19:00',
			{ always_on: '1', schedule_enabled: '1', schedule_mode: 'workweek' });

		this.customScheduleFields = {};
		WEEKDAYS.forEach(L.bind(function(day) {
			this.customScheduleFields[day[0]] = this.addScheduleRange(s,
				'_schedule_' + day[0] + '_range', day[1], 'schedule_' + day[0],
				true, true, '08:00', '23:00',
				{ always_on: '1', schedule_enabled: '1', schedule_mode: 'weekly' });
		}, this));

		o = s.option(form.DummyValue, '_schedule_copy', _('Copy one day'));
		o.depends({ always_on: '1', schedule_enabled: '1', schedule_mode: 'weekly' });
		o.renderWidget = L.bind(function() {
			return this.renderScheduleCopy();
		}, this);

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
