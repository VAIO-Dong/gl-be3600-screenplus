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
		_('Use a six-digit colour value, for example #37F59A.');
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
			reject(new Error(_('This image could not be opened. Please choose another file.')));
		};
		image.src = objectUrl;
	});
}

function rgb565Preview(base64) {
	var raw = atob(base64.replace(/\s/g, ''));
	if (raw.length !== WIDTH * HEIGHT * 2)
		throw new Error(_('This background could not be previewed. Try uploading it again.'));
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
			ui.addNotification(null, E('p', {}, _('Choose an image smaller than 10 MB.')));
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
				throw new Error(result && result.stderr || _('The background could not be saved.'));
			self.setBackgroundPreview(page, convertedPreview);
			self.setBackgroundModeControl(page === 'global' ? 'global' : 'page');
			ui.addNotification(null, E('p', {},
				_('Your new background is now in use.')), 'info');
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
				throw new Error(result && result.stderr || _('The background could not be removed.'));
			self.setBackgroundPreview(page, null);
			if (page === 'global')
				self.setBackgroundModeControl('page');
			ui.addNotification(null, E('p', {}, _('The custom background has been removed.')), 'info');
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
		}, [ _('Remove background') ]);
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
		var map = new form.Map('screenplus', _('Theme and backgrounds'),
			_('Personalise the colours and background pictures used across ScreenPlus. Clear a colour value to restore the original.'));
		var section = map.section(form.NamedSection, 'appearance', 'appearance', _('Colours'));
		section.anonymous = true;
		section.addremove = false;
		section.tab('palette', _('Theme colours'));
		section.tab('states', _('Connection colours'));
		section.tab('backgrounds', _('Background settings'));

		var option = section.taboption('palette', form.Value, 'accent', _('Theme colour'));
		option.default = '#37f59a';
		option.rmempty = true;
		option.validate = validateColour;
		option.description = _('Used for headings, highlights, active switches and healthy connections.');

		option = section.taboption('palette', form.Value, 'primary', _('Main text colour'));
		option.default = '#ffffff';
		option.rmempty = true;
		option.validate = validateColour;
		option.description = _('Used for the clock, important values, addresses, Wi-Fi names and passwords.');

		option = section.taboption('palette', form.Value, 'secondary', _('Secondary text colour'));
		option.default = '#dcecff';
		option.rmempty = true;
		option.validate = validateColour;
		option.description = _('Used for labels, units, supporting information and unavailable connections.');

		option = section.taboption('palette', form.Value, 'background', _('Page background colour'));
		option.default = '#030912';
		option.rmempty = true;
		option.validate = validateColour;
		option.description = _('Shown behind each page when no custom picture is used.');

		option = section.taboption('palette', form.Value, 'border', _('Divider colour'));
		option.default = '#3b424a';
		option.rmempty = true;
		option.validate = validateColour;
		option.description = _('Used for separators and the track of an inactive switch.');

		option = section.taboption('states', form.Value, 'standby', _('Available but not enabled'));
		option.default = '#4b9fff';
		option.rmempty = true;
		option.validate = validateColour;
		option.description = _('The connection or device is available, but is not currently enabled.');

		option = section.taboption('states', form.Value, 'warning', _('Connected without internet'));
		option.default = '#ffdc55';
		option.rmempty = true;
		option.validate = validateColour;
		option.description = _('The connection is enabled and linked, but cannot reach the internet.');

		option = section.taboption('states', form.Value, 'error', _('Error state'));
		option.default = '#ff5c70';
		option.rmempty = true;
		option.validate = validateColour;
		option.description = _('Used when a service reports a problem.');

		option = section.taboption('backgrounds', form.Value, 'overlay_opacity', _('Background dimming'));
		option.datatype = 'range(0,100)';
		option.default = '35';
		option.rmempty = true;
		option.description = _('Darkens custom pictures so text stays easy to read. A higher value looks darker.');

		option = section.taboption('backgrounds', form.ListValue, 'background_mode', _('Background style'));
		option.value('page', _('Different picture on each page'));
		option.value('global', _('Same picture on every page'));
		option.default = 'page';
		option.rmempty = false;
		option.description = _('Uploading a picture below automatically selects the matching style.');

		return map.render().then(L.bind(function(mapNode) {
			return E([], [
				mapNode,
				E('div', { 'class': 'cbi-map' }, [
					E('h2', {}, [ _('Background pictures') ]),
					E('div', { 'class': 'cbi-map-descr' }, [
						_('For an exact fit, use a 284 × 76 px PNG, JPEG, WebP or BMP image. Other sizes are centre-cropped to fit; the maximum file size is 10 MB. New pictures take effect immediately.')
					]),
					E('div', { 'class': 'cbi-section' }, [
						this.renderUploader('global', _('All pages')),
						this.renderUploader('home', _('Home')),
						this.renderUploader('traffic', _('Live traffic')),
						this.renderUploader('status', _('System status')),
						this.renderUploader('wifi', _('Wi-Fi')),
						this.renderUploader('network', _('Network connections')),
						this.renderUploader('openclash', _('OpenClash'))
					])
				])
			]);
		}, this));
	}
});
