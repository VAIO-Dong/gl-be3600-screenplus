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

return view.extend({
	handleBackgroundFile: function(page, event) {
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
		return convertImage(file).then(function(result) {
			var preview = document.getElementById('screenplus-preview-' + page);
			if (preview)
				preview.src = result.preview;
			return fs.write('/tmp/screenplus-background-' + page + '.b64', result.base64);
		}).then(function() {
			return fs.exec(BACKGROUND_HELPER, [ 'install', page ]);
		}).then(function(result) {
			if (!result || result.code !== 0)
				throw new Error(result && result.stderr || _('Background installation failed.'));
			ui.addNotification(null, E('p', {},
				_('Background installed. Select “Use uploaded image” on the Pages tab and save.')), 'info');
		}).catch(function(error) {
			ui.addNotification(null, E('p', {}, error.message));
		}).finally(function() {
			input.disabled = false;
			input.value = '';
		});
	},

	handleBackgroundRemove: function(page, event) {
		var button = event.currentTarget;
		button.disabled = true;
		return fs.exec(BACKGROUND_HELPER, [ 'remove', page ]).then(function(result) {
			if (!result || result.code !== 0)
				throw new Error(result && result.stderr || _('Background removal failed.'));
			var preview = document.getElementById('screenplus-preview-' + page);
			if (preview)
				preview.removeAttribute('src');
			ui.addNotification(null, E('p', {}, _('Uploaded background removed.')), 'info');
		}).catch(function(error) {
			ui.addNotification(null, E('p', {}, error.message));
		}).finally(function() {
			button.disabled = false;
		});
	},

	renderUploader: function(page, title) {
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
				E('div', { 'style': 'margin-top:.5em' }, [
					E('img', {
						'id': 'screenplus-preview-' + page,
						'width': WIDTH,
						'height': HEIGHT,
						'alt': _('Converted 284 × 76 preview'),
						'style': 'max-width:100%;height:auto;border:2px solid #4f87b8;background:#030912'
					})
				])
			])
		]);
	},

	render: function() {
		var map = new form.Map('screenplus', _('Appearance'),
			_('Choose the high-contrast screen colours. Changes apply when the service reloads.'));
		var section = map.section(form.NamedSection, 'appearance', 'appearance', _('Theme'));
		section.anonymous = true;
		section.addremove = false;

		var option = section.option(form.Value, 'primary', _('Primary text colour'));
		option.default = '#ffffff';
		option.rmempty = false;
		option.validate = validateColour;

		option = section.option(form.Value, 'secondary', _('Secondary text colour'));
		option.default = '#dcecff';
		option.rmempty = false;
		option.validate = validateColour;

		option = section.option(form.Value, 'accent', _('Theme / accent colour'));
		option.default = '#37f59a';
		option.rmempty = false;
		option.validate = validateColour;

		option = section.option(form.Value, 'background', _('Background colour'));
		option.default = '#030912';
		option.rmempty = false;
		option.validate = validateColour;

		option = section.option(form.Value, 'surface', _('Surface colour'));
		option.default = '#0a1828';
		option.rmempty = false;
		option.validate = validateColour;

		option = section.option(form.Value, 'border', _('Divider / inactive colour'));
		option.default = '#4f87b8';
		option.rmempty = false;
		option.validate = validateColour;

		option = section.option(form.Value, 'warning', _('Warning colour'));
		option.default = '#ffdc55';
		option.rmempty = false;
		option.validate = validateColour;

		option = section.option(form.Value, 'error', _('Error colour'));
		option.default = '#ff5c70';
		option.rmempty = false;
		option.validate = validateColour;

		option = section.option(form.Value, 'overlay_opacity', _('Background overlay opacity'));
		option.datatype = 'range(0,100)';
		option.default = '35';
		option.rmempty = false;
		option.description = _('Percentage used when a custom background image is selected.');

		return map.render().then(L.bind(function(mapNode) {
			return E([], [
				mapNode,
				E('div', { 'class': 'cbi-map' }, [
					E('h2', {}, [ _('Custom backgrounds') ]),
					E('div', { 'class': 'cbi-map-descr' }, [
						_('Images are cropped in the browser and stored as a fixed 284 × 76 RGB565 asset. No arbitrary server path is accepted.')
					]),
					E('div', { 'class': 'cbi-section' }, [
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
