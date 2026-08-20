'use strict';
'require view';
'require form';
'require fs';
'require ui';

var BACKGROUND_HELPER = '/usr/libexec/screenplus-background';
var WIDTH = 284;
var HEIGHT = 76;

function validateColour(section, value) {
	return /^#[0-9a-fA-F]{6}$/.test(value) || _('Enter a colour as #RRGGBB.');
}

function convertImage(file) {
	return new Promise(function(resolve, reject) {
		var image = new Image();
		var objectUrl = URL.createObjectURL(file);
		image.onload = function() {
			try {
				var canvas = document.createElement('canvas');
				canvas.width = WIDTH;
				canvas.height = HEIGHT;
				var context = canvas.getContext('2d', { alpha: false });
				var scale = Math.max(WIDTH / image.naturalWidth, HEIGHT / image.naturalHeight);
				var drawWidth = image.naturalWidth * scale;
				var drawHeight = image.naturalHeight * scale;
				context.fillStyle = '#000000';
				context.fillRect(0, 0, WIDTH, HEIGHT);
				context.drawImage(image, (WIDTH - drawWidth) / 2, (HEIGHT - drawHeight) / 2,
					drawWidth, drawHeight);
				var rgba = context.getImageData(0, 0, WIDTH, HEIGHT).data;
				var pieces = [];
				var piece = '';
				for (var index = 0; index < rgba.length; index += 4) {
					var rgb565 = ((rgba[index] & 0xf8) << 8) |
						((rgba[index + 1] & 0xfc) << 3) | (rgba[index + 2] >> 3);
					piece += String.fromCharCode(rgb565 & 0xff, rgb565 >> 8);
					if (piece.length >= 8192) {
						pieces.push(piece);
						piece = '';
					}
				}
				pieces.push(piece);
				resolve({
					base64: btoa(pieces.join('')),
					preview: canvas.toDataURL('image/png')
				});
			} catch (error) {
				reject(error);
			} finally {
				URL.revokeObjectURL(objectUrl);
			}
		};
		image.onerror = function() {
			URL.revokeObjectURL(objectUrl);
			reject(new Error(_('The selected file is not a readable image.')));
		};
		image.src = objectUrl;
	});
}

function rgb565Preview(base64) {
	var raw = atob(base64.replace(/\s/g, ''));
	if (raw.length !== WIDTH * HEIGHT * 2)
		throw new Error(_('The installed background has an invalid size.'));
	var canvas = document.createElement('canvas');
	canvas.width = WIDTH;
	canvas.height = HEIGHT;
	var context = canvas.getContext('2d', { alpha: false });
	var image = context.createImageData(WIDTH, HEIGHT);
	for (var pixel = 0, offset = 0; offset < raw.length; pixel += 4, offset += 2) {
		var value = raw.charCodeAt(offset) | (raw.charCodeAt(offset + 1) << 8);
		var red = (value >> 11) & 0x1f;
		var green = (value >> 5) & 0x3f;
		var blue = value & 0x1f;
		image.data[pixel] = (red << 3) | (red >> 2);
		image.data[pixel + 1] = (green << 2) | (green >> 4);
		image.data[pixel + 2] = (blue << 3) | (blue >> 2);
		image.data[pixel + 3] = 255;
	}
	context.putImageData(image, 0, 0);
	return canvas.toDataURL('image/png');
}

return view.extend({
	setBackgroundPreview: function(page, source) {
		var preview = document.getElementById('screenplus-preview-' + page);
		var container = document.getElementById('screenplus-preview-container-' + page);
		if (!preview || !container)
			return;
		if (source) {
			preview.src = source;
			container.style.display = '';
		} else {
			preview.removeAttribute('src');
			container.style.display = 'none';
		}
	},

	loadBackgroundPreview: function(page, preview, container) {
		return fs.exec(BACKGROUND_HELPER, [ 'preview', page ]).then(function(result) {
			if (!result || result.code !== 0 || !result.stdout)
				return;
			preview.src = rgb565Preview(result.stdout);
			container.style.display = '';
		}).catch(function() {
			/* A missing or invalid installed image simply has no preview. */
		});
	},

	setBackgroundModeControl: function(mode) {
		var control = document.querySelector('[id$=".appearance.background_mode"]');
		if (control) {
			control.value = mode;
			control.dispatchEvent(new Event('change', { bubbles: true }));
		}
	},

	handleBackgroundFile: function(page, event) {
		var self = this;
		var input = event.currentTarget;
		var file = input.files && input.files[0];
		if (!file)
			return;
		if (file.size > 10 * 1024 * 1024) {
			ui.addNotification(null, E('p', {}, _('The source image must be 10 MiB or smaller.')));
			input.value = '';
			return;
		}
		input.disabled = true;
		var convertedPreview = null;
		return convertImage(file).then(function(result) {
			convertedPreview = result.preview;
			return fs.write('/tmp/screenplus-background-' + page + '.b64', result.base64);
		}).then(function() {
			return fs.exec(BACKGROUND_HELPER, [ 'install', page ]);
		}).then(function(result) {
			if (!result || result.code !== 0)
				throw new Error(result && result.stderr || _('Background installation failed.'));
			self.setBackgroundPreview(page, convertedPreview);
			self.setBackgroundModeControl(page === 'global' ? 'global' : 'page');
			ui.addNotification(null, E('p', {},
				_('Background installed and applied immediately.')), 'info');
		}).catch(function(error) {
			ui.addNotification(null, E('p', {}, error.message));
		}).finally(function() {
			input.disabled = false;
			input.value = '';
		});
	},

	handleBackgroundRemove: function(page, event) {
		var self = this;
		var button = event.currentTarget;
		button.disabled = true;
		return fs.exec(BACKGROUND_HELPER, [ 'remove', page ]).then(function(result) {
			if (!result || result.code !== 0)
				throw new Error(result && result.stderr || _('Background removal failed.'));
			self.setBackgroundPreview(page, null);
			if (page === 'global')
				self.setBackgroundModeControl('page');
			ui.addNotification(null, E('p', {}, _('Uploaded background removed.')), 'info');
		}).catch(function(error) {
			ui.addNotification(null, E('p', {}, error.message));
		}).finally(function() {
			button.disabled = false;
		});
	},

	renderUploader: function(page, title) {
		var preview = E('img', {
			'id': 'screenplus-preview-' + page,
			'width': WIDTH,
			'height': HEIGHT,
			'alt': _('Converted 284 × 76 preview'),
			'style': 'max-width:100%;height:auto;border:2px solid #4f87b8;background:#030912'
		});
		var previewContainer = E('div', {
			'id': 'screenplus-preview-container-' + page,
			'style': 'display:none;margin-top:.5em'
		}, [ preview ]);
		this.loadBackgroundPreview(page, preview, previewContainer);
		return E('div', { 'class': 'cbi-value' }, [
			E('label', { 'class': 'cbi-value-title' }, [ title ]),
			E('div', { 'class': 'cbi-value-field' }, [
				E('input', {
					'type': 'file',
					'accept': 'image/png,image/jpeg,image/webp,image/bmp',
					'change': ui.createHandlerFn(this, 'handleBackgroundFile', page)
				}),
				' ',
				E('button', {
					'class': 'btn cbi-button cbi-button-negative',
					'click': ui.createHandlerFn(this, 'handleBackgroundRemove', page)
				}, [ _('Remove') ]),
				previewContainer
			])
		]);
	},

	render: function() {
		var map = new form.Map('screenplus', _('Appearance'),
			_('Text and layout colours are followed by connection-state colours: grey means missing, blue means available but disabled, yellow means enabled without internet, green means healthy, and red means a fault. Changes apply when the service reloads.'));
		var section = map.section(form.NamedSection, 'appearance', 'appearance', _('Theme'));
		section.anonymous = true;
		section.addremove = false;
		section.tab('layout', _('Text and layout'));
		section.tab('states', _('Connection states'));
		section.tab('backgrounds', _('Background images'));

		var option = section.taboption('layout', form.Value, 'primary', _('Primary text colour'));
		option.default = '#ffffff';
		option.rmempty = false;
		option.validate = validateColour;

		option = section.taboption('layout', form.Value, 'secondary', _('Secondary text colour'));
		option.default = '#dcecff';
		option.rmempty = false;
		option.validate = validateColour;

		option = section.taboption('layout', form.Value, 'background', _('Background colour'));
		option.default = '#030912';
		option.rmempty = false;
		option.validate = validateColour;

		option = section.taboption('layout', form.Value, 'border', _('Divider colour'));
		option.default = '#4f87b8';
		option.rmempty = false;
		option.validate = validateColour;

		option = section.taboption('states', form.Value, 'accent', _('Healthy / accent colour'));
		option.default = '#37f59a';
		option.rmempty = false;
		option.validate = validateColour;

		option = section.taboption('states', form.Value, 'absent', _('Missing connection colour'));
		option.default = '#8a939f';
		option.rmempty = false;
		option.validate = validateColour;

		option = section.taboption('states', form.Value, 'standby', _('Available but disabled colour'));
		option.default = '#4b9fff';
		option.rmempty = false;
		option.validate = validateColour;

		option = section.taboption('states', form.Value, 'warning', _('Enabled but offline colour'));
		option.default = '#ffdc55';
		option.rmempty = false;
		option.validate = validateColour;

		option = section.taboption('states', form.Value, 'error', _('Fault colour'));
		option.default = '#ff5c70';
		option.rmempty = false;
		option.validate = validateColour;

		option = section.taboption('backgrounds', form.Value, 'overlay_opacity', _('Background overlay opacity'));
		option.datatype = 'range(0,100)';
		option.default = '35';
		option.rmempty = false;
		option.description = _('Percentage used when a custom background image is selected.');

		option = section.taboption('backgrounds', form.ListValue, 'background_mode', _('Background image mode'));
		option.value('page', _('Use a separate background for each page'));
		option.value('global', _('Use one background for all pages'));
		option.default = 'page';
		option.rmempty = false;
		option.description = _('Uploading a global or page image automatically selects the matching mode and applies it immediately.');

		return map.render().then(L.bind(function(mapNode) {
			return E([], [
				mapNode,
				E('div', { 'class': 'cbi-map' }, [
					E('h2', {}, [ _('Custom backgrounds') ]),
					E('div', { 'class': 'cbi-map-descr' }, [
						_('Images are cropped to 284 × 76 in the browser. Uploading an image immediately applies it; no second selection is required.')
					]),
					E('div', { 'class': 'cbi-section' }, [
						this.renderUploader('global', _('Global background (all pages)')),
						this.renderUploader('home', _('Home / clock')),
						this.renderUploader('status', _('Device status')),
						this.renderUploader('traffic', _('Network traffic')),
						this.renderUploader('network', _('Network')),
						this.renderUploader('wifi', _('Wi-Fi credentials')),
						this.renderUploader('openclash', _('OpenClash'))
					])
				])
			]);
		}, this));
	}
});
