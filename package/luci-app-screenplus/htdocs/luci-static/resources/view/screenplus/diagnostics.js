'use strict';
'require view';
'require fs';
'require ui';

var DIAGNOSTICS = '/usr/libexec/screenplus-diagnostics';
var INIT = '/etc/init.d/screenplus';

function runDiagnostic(action) {
	return fs.exec(DIAGNOSTICS, [ action ]).then(function(result) {
		return result && result.code === 0 ? result.stdout.trim() :
			(result && result.stderr || _('Diagnostic command failed.'));
	});
}

return view.extend({
	load: function() {
		return Promise.all([
			runDiagnostic('status'),
			runDiagnostic('metrics'),
			runDiagnostic('system'),
			runDiagnostic('logs')
		]);
	},

	handleAction: function(action, event) {
		var button = event.currentTarget;
		button.disabled = true;
		button.classList.add('spinning');
		return fs.exec(INIT, [ action ]).then(function(result) {
			if (!result || result.code !== 0)
				throw new Error(result && result.stderr || _('Service action failed.'));
			ui.addNotification(null, E('p', {}, _('ScreenPlus service action completed.')), 'info');
			window.setTimeout(function() { window.location.reload(); }, 800);
		}).catch(function(error) {
			ui.addNotification(null, E('p', {}, error.message));
		}).finally(function() {
			button.disabled = false;
			button.classList.remove('spinning');
		});
	},

	renderBlock: function(title, value) {
		return E('div', { 'class': 'cbi-section' }, [
			E('h3', {}, [ title ]),
			E('pre', { 'style': 'white-space:pre-wrap;overflow-wrap:anywhere' }, [ value || '--' ])
		]);
	},

	render: function(data) {
		return E([], [
			E('h2', {}, [ _('ScreenPlus diagnostics') ]),
			E('div', { 'class': 'cbi-map-descr' }, [
				_('Live samples intentionally omit the Wi-Fi password.')
			]),
			E('div', { 'class': 'cbi-section' }, [
				E('button', {
					'class': 'btn cbi-button cbi-button-action',
					'click': ui.createHandlerFn(this, 'handleAction', 'reload')
				}, [ _('Reload configuration') ]),
				' ',
				E('button', {
					'class': 'btn cbi-button cbi-button-action',
					'click': ui.createHandlerFn(this, 'handleAction', 'restart')
				}, [ _('Restart service') ])
			]),
			this.renderBlock(_('Service and hardware'), data[0]),
			this.renderBlock(_('Device metrics'), data[1]),
			this.renderBlock(_('Connectivity snapshot'), data[2]),
			this.renderBlock(_('Recent ScreenPlus logs'), data[3])
		]);
	},

	handleSaveApply: null,
	handleSave: null,
	handleReset: null
});
