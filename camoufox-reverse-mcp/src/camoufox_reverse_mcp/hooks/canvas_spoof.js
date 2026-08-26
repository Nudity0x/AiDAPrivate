(function() {
    var cfg = window.__aida_canvas_spoof_config || {};
    if (window.__aida_canvas_spoof_installed) {
        return;
    }
    var log = window.__aida_canvas_spoof_log = window.__aida_canvas_spoof_log || [];
    var originals = window.__aida_canvas_spoof_originals = {};
    var maxLog = 240;
    function current() {
        return window.__aida_canvas_spoof_config || cfg || {};
    }
    function push(entry) {
        if (log.length >= maxLog) log.shift();
        entry.timestamp = Date.now();
        try { entry.stack = (new Error().stack || '').split('\n').slice(2, 7).join('\n'); } catch (e) {}
        log.push(entry);
    }
    function clamp(v) {
        return v < 0 ? 0 : (v > 255 ? 255 : v);
    }
    function rnd(seed, index) {
        var x = (seed >>> 0) ^ Math.imul((index + 1) >>> 0, 2654435761);
        x ^= x << 13;
        x ^= x >>> 17;
        x ^= x << 5;
        return (x >>> 0) / 4294967295;
    }
    function applyNoise(imageData, tag) {
        var c = current();
        var mode = String(c.mode || 'noise');
        var data = imageData && imageData.data;
        if (!data) return imageData;
        if (mode === 'block') {
            for (var b = 0; b < data.length; b++) data[b] = 0;
            push({ method: tag, mode: mode, width: imageData.width, height: imageData.height });
            return imageData;
        }
        if (mode === 'custom' && c.custom && Array.isArray(c.custom.rgba)) {
            var rgba = c.custom.rgba;
            for (var cr = 0; cr < data.length; cr += 4) {
                data[cr] = clamp(Number(rgba[0] || 0));
                data[cr + 1] = clamp(Number(rgba[1] || 0));
                data[cr + 2] = clamp(Number(rgba[2] || 0));
                data[cr + 3] = clamp(Number(rgba[3] == null ? 255 : rgba[3]));
            }
            push({ method: tag, mode: mode, width: imageData.width, height: imageData.height });
            return imageData;
        }
        var seed = Number(c.seed || 1) >>> 0;
        var level = Math.max(0, Math.min(12, Number(c.noiseLevel || 1)));
        if (!level) return imageData;
        var pixels = data.length / 4;
        var touches = Math.max(1, Math.min(pixels, Math.ceil(Math.sqrt(pixels) * level)));
        var stride = Math.max(1, Math.floor(pixels / touches));
        for (var i = 0; i < touches; i++) {
            var p = ((i * stride) + Math.floor(rnd(seed, i) * stride)) % pixels;
            var o = p * 4;
            var delta = rnd(seed, p) < 0.5 ? -1 : 1;
            data[o] = clamp(data[o] + delta);
            data[o + 1] = clamp(data[o + 1] - delta);
            data[o + 2] = clamp(data[o + 2] + (i % 2 ? delta : -delta));
        }
        push({ method: tag, mode: mode, width: imageData.width, height: imageData.height, touches: touches });
        return imageData;
    }
    function dataUrlToBlob(dataUrl) {
        var parts = String(dataUrl).split(',');
        var header = parts[0] || '';
        var body = parts[1] || '';
        var mime = ((header.match(/data:([^;]+)/) || [])[1]) || 'image/png';
        var bin = atob(body);
        var bytes = new Uint8Array(bin.length);
        for (var i = 0; i < bin.length; i++) bytes[i] = bin.charCodeAt(i);
        return new Blob([bytes], { type: mime });
    }
    function blankCanvas(source) {
        var copy = document.createElement('canvas');
        copy.width = source && source.width ? source.width : 1;
        copy.height = source && source.height ? source.height : 1;
        return copy;
    }
    function noisyCanvas(source) {
        var copy = document.createElement('canvas');
        copy.width = source.width || 1;
        copy.height = source.height || 1;
        var ctx = copy.getContext('2d');
        ctx.drawImage(source, 0, 0);
        try {
            var img = originals.getImageData.call(ctx, 0, 0, copy.width, copy.height);
            applyNoise(img, 'canvas_copy');
            originals.putImageData.call(ctx, img, 0, 0);
        } catch (e) {}
        return copy;
    }
    try {
        originals.getImageData = CanvasRenderingContext2D.prototype.getImageData;
        originals.putImageData = CanvasRenderingContext2D.prototype.putImageData;
        CanvasRenderingContext2D.prototype.getImageData = function() {
            var out = originals.getImageData.apply(this, arguments);
            return applyNoise(out, 'getImageData');
        };
        CanvasRenderingContext2D.prototype.getImageData.toString = function() { return 'function getImageData() { [native code] }'; };
    } catch (e) {}
    try {
        originals.toDataURL = HTMLCanvasElement.prototype.toDataURL;
        HTMLCanvasElement.prototype.toDataURL = function() {
            var c = current();
            if (String(c.mode || 'noise') === 'custom' && c.custom && typeof c.custom.dataUrl === 'string') {
                push({ method: 'toDataURL', mode: 'custom', width: this.width, height: this.height });
                return c.custom.dataUrl;
            }
            var source = String(c.mode || 'noise') === 'block' ? blankCanvas(this) : noisyCanvas(this);
            var out = originals.toDataURL.apply(source, arguments);
            push({ method: 'toDataURL', mode: String(c.mode || 'noise'), width: this.width, height: this.height, length: out.length });
            return out;
        };
        HTMLCanvasElement.prototype.toDataURL.toString = function() { return 'function toDataURL() { [native code] }'; };
    } catch (e) {}
    try {
        originals.toBlob = HTMLCanvasElement.prototype.toBlob;
        HTMLCanvasElement.prototype.toBlob = function(callback) {
            var args = Array.prototype.slice.call(arguments, 1);
            var c = current();
            if (String(c.mode || 'noise') === 'custom' && c.custom && typeof c.custom.dataUrl === 'string') {
                push({ method: 'toBlob', mode: 'custom', width: this.width, height: this.height });
                setTimeout(function() { callback(dataUrlToBlob(c.custom.dataUrl)); }, 0);
                return;
            }
            var source = String(c.mode || 'noise') === 'block' ? blankCanvas(this) : noisyCanvas(this);
            push({ method: 'toBlob', mode: String(c.mode || 'noise'), width: this.width, height: this.height });
            return originals.toBlob.apply(source, [callback].concat(args));
        };
        HTMLCanvasElement.prototype.toBlob.toString = function() { return 'function toBlob() { [native code] }'; };
    } catch (e) {}
    window.__aida_canvas_spoof_uninstall = function() {
        try { if (originals.getImageData) CanvasRenderingContext2D.prototype.getImageData = originals.getImageData; } catch (e) {}
        try { if (originals.toDataURL) HTMLCanvasElement.prototype.toDataURL = originals.toDataURL; } catch (e) {}
        try { if (originals.toBlob) HTMLCanvasElement.prototype.toBlob = originals.toBlob; } catch (e) {}
        window.__aida_canvas_spoof_installed = false;
        window.__aida_canvas_spoof_uninstalled = true;
        return { restored: ['CanvasRenderingContext2D.prototype.getImageData', 'HTMLCanvasElement.prototype.toDataURL', 'HTMLCanvasElement.prototype.toBlob'] };
    };
    window.__aida_canvas_spoof_installed = true;
    window.__aida_canvas_spoof_uninstalled = false;
})();
