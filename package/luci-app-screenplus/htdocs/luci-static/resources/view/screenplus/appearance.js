'use strict';
'require view';
'require form';
'require fs';
'require ui';

var BACKGROUND_HELPER = '/usr/libexec/screenplus-background';
var WIDTH = 284;
var HEIGHT = 76;

function validateColour(section, value) {
	return value == null || value === '' || /^#[0-9a-fA-F]{6}$/.test(value) ||
		_('Enter a colour as #RRGGBB.');
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
		var container = document.getElementById('screenplus-preview-container-' + page);
		var removeButton = document.getElementById('screenplus-remove-' + page);
		if (!container || !removeButton)
			return;
		this.renderBackgroundPreview(page, container, removeButton, source);
	},

	renderBackgroundPreview: function(page, container, removeButton, source) {
		container.dataset.previewSource = source || '';
		while (container.firstChild)
			container.removeChild(container.firstChild);
		container.style.display = 'none';
		removeButton.style.display = 'none';
		if (!source)
			return;

		/* Keep the image detached and the controls hidden until the browser has
		 * decoded it. A missing or malformed asset can therefore never render a
		 * broken-image placeholder, even briefly. */
		var image = E('img', {
			'id': 'screenplus-preview-' + page,
			'width': WIDTH,
			'height': HEIGHT,
			'alt': '',
			'style': 'max-width:100%;height:auto;border:2px solid #3b424a;background:#030912'
		});
		image.onload = function() {
			if (container.dataset.previewSource !== source)
				return;
			container.appendChild(image);
			container.style.display = '';
			removeButton.style.display = '';
		};
		image.onerror = function() {
			if (container.dataset.previewSource !== source)
				return;
			container.dataset.previewSource = '';
			container.style.display = 'none';
			removeButton.style.display = 'none';
		};
		image.src = source;
	},

	loadBackgroundPreview: function(page, container, removeButton) {
		var self = this;
		self.renderBackgroundPreview(page, container, removeButton, null);
		return fs.exec(BACKGROUND_HELPER, [ 'preview', page ]).then(function(result) {
			if (!result || result.code !== 0 || !result.stdout) {
				self.renderBackgroundPreview(page, container, removeButton, null);
				return;
			}
			self.renderBackgroundPreview(page, container, removeButton,
				rgb565Preview(result.stdout));
		}).catch(function() {
			self.renderBackgroundPreview(page, container, removeButton, null);
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
		var previewContainer = E('div', {
			'id': 'screenplus-preview-container-' + page,
			'style': 'display:none;margin-top:.5em'
		});
		var removeButton = E('button', {
			'id': 'screenplus-remove-' + page,
			'type': 'button',
			'class': 'btn cbi-button cbi-button-negative',
			'style': 'display:none',
			'click': ui.createHandlerFn(this, 'handleBackgroundRemove', page)
		}, [ _('Remove') ]);
		this.loadBackgroundPreview(page, previewContainer, removeButton);
		return E('div', { 'class': 'cbi-value' }, [
			E('label', { 'class': 'cbi-value-title' }, [ title ]),
			E('div', { 'class': 'cbi-value-field' }, [
				E('input', {
					'type': 'file',
					'accept': 'image/png,image/jpeg,image/webp,image/bmp',
					'change': ui.createHandlerFn(this, 'handleBackgroundFile', page)
				}),
				' ',
				removeButton,
				previewContainer
			])
		]);
	},

	render: function() {
		var map = new form.Map('screenplus', _('Appearance'),
			_('Theme, primary and secondary colours define the visual hierarchy. Healthy states reuse the theme colour and missing connections reuse the secondary colour. The remaining state colours are only used for their named conditions. Leave a value empty to restore its built-in default. Changes apply when the service reloads.'));
		var section = map.section(form.NamedSection, 'appearance', 'appearance', _('Theme'));
		section.anonymous = true;
		section.addremove = false;
		section.tab('palette', _('Core palette'));
		section.tab('states', _('Connection states'));
		section.tab('backgrounds', _('Background images'));

		var option = section.taboption('palette', form.Value, 'accent', _('Theme colour'));
		option.default = '#37f59a';
		option.rmempty = true;
		option.validate = validateColour;
		option.description = _('Page headings, the home accent, enabled switches, download graphs and healthy or active states.');

		option = section.taboption('palette', form.Value, 'primary', _('Primary colour'));
		option.default = '#ffffff';
		option.rmempty = true;
		option.validate = validateColour;
		option.description = _('Clock, main values, addresses, SSIDs, passwords and other high-priority content.');

		option = section.taboption('palette', form.Value, 'secondary', _('Secondary colour'));
		option.default = '#dcecff';
		option.rmempty = true;
		option.validate = validateColour;
		option.description = _('Supporting labels, units, upload graphs, disabled Wi-Fi and missing connections.');

		option = section.taboption('palette', form.Value, 'background', _('Background colour'));
		option.default = '#030912';
		option.rmempty = true;
		option.validate = validateColour;
		option.description = _('Base colour behind every page when no custom image covers it.');

		option = section.taboption('palette', form.Value, 'border', _('Divider colour'));
		option.default = '#3b424a';
		option.rmempty = true;
		option.validate = validateColour;
		option.description = _('Section separators and the inactive switch track.');

		option = section.taboption('states', form.Value, 'standby', _('Available but disabled colour'));
		option.default = '#4b9fff';
		option.rmempty = true;
		option.validate = validateColour;
		option.description = _('A connection or device is present and configured, but disabled.');

		option = section.taboption('states', form.Value, 'warning', _('Enabled but offline colour'));
		option.default = '#ffdc55';
		option.rmempty = true;
		option.validate = validateColour;
		option.description = _('A connection is enabled or linked but has no working internet access.');

		option = section.taboption('states', form.Value, 'error', _('Fault colour'));
		option.default = '#ff5c70';
		option.rmempty = true;
		option.validate = validateColour;
		option.description = _('A service reports an explicit error or failed state.');

		option = section.taboption('backgrounds', form.Value, 'overlay_opacity', _('Background overlay opacity'));
		option.datatype = 'range(0,100)';
		option.default = '35';
		option.rmempty = true;
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
						this.renderUploader('traffic', _('Network traffic')),
						this.renderUploader('status', _('Device status')),
						this.renderUploader('wifi', _('Wi-Fi credentials')),
						this.renderUploader('network', _('LAN / WAN connections')),
						this.renderUploader('openclash', _('OpenClash'))
					])
				])
			]);
		}, this));
	}
});
