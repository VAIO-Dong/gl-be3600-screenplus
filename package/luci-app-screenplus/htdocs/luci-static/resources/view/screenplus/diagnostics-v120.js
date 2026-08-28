'use strict';
'require view';
'require fs';
'require ui';

var DIAGNOSTICS = '/usr/libexec/screenplus-diagnostics';
var INIT = '/etc/init.d/screenplus';

function runDiagnostic(action) {
	return fs.exec(DIAGNOSTICS, [ action ]).then(function(result) {
		return result && result.code === 0 ? result.stdout.trim() :
			(result && result.stderr || _('This information is temporarily unavailable.'));
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
				throw new Error(result && result.stderr || _('ScreenPlus could not complete this action.'));
			ui.addNotification(null, E('p', {}, _('ScreenPlus is ready.')), 'info');
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
			E('h2', {}, [ _('Status and support') ]),
			E('div', { 'class': 'cbi-map-descr' }, [
				_('Check whether ScreenPlus is working normally or refresh it after troubleshooting.')
			]),
			E('div', { 'class': 'cbi-section' }, [
				E('button', {
					'class': 'btn cbi-button cbi-button-action',
					'click': ui.createHandlerFn(this, 'handleAction', 'reload')
				}, [ _('Refresh ScreenPlus') ]),
				' ',
				E('button', {
					'class': 'btn cbi-button cbi-button-action',
					'click': ui.createHandlerFn(this, 'handleAction', 'restart')
				}, [ _('Restart ScreenPlus') ])
			]),
			this.renderBlock(_('Screen status'), data[0]),
			this.renderBlock(_('Performance snapshot'), data[1]),
			this.renderBlock(_('Network snapshot'), data[2]),
			this.renderBlock(_('Recent activity'), data[3])
		]);
	},

	handleSaveApply: null,
	handleSave: null,
	handleReset: null
});
