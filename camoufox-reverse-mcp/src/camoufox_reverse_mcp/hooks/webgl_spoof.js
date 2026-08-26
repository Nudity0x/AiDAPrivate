(function() {
    var cfg = window.__aida_webgl_spoof_config || {};
    if (window.__aida_webgl_spoof_installed) {
        return;
    }
    var log = window.__aida_webgl_spoof_log = window.__aida_webgl_spoof_log || [];
    var originals = window.__aida_webgl_spoof_originals = [];
    var maxLog = 240;
    var constants = {
        VENDOR: 0x1F00,
        RENDERER: 0x1F01,
        UNMASKED_VENDOR_WEBGL: 0x9245,
        UNMASKED_RENDERER_WEBGL: 0x9246
    };
    function current() {
        return window.__aida_webgl_spoof_config || cfg || {};
    }
    function push(entry) {
        if (log.length >= maxLog) log.shift();
        entry.timestamp = Date.now();
        try { entry.stack = (new Error().stack || '').split('\n').slice(2, 7).join('\n'); } catch (e) {}
        log.push(entry);
    }
    function preview(value) {
        try {
            var text = String(value);
            return text.length > 180 ? text.slice(0, 180) : text;
        } catch (e) {
            return '';
        }
    }
    function spoofedParameter(param, originalValue) {
        var c = current();
        var custom = c.custom && c.custom.parameters ? c.custom.parameters : {};
        if (Object.prototype.hasOwnProperty.call(custom, String(param))) return custom[String(param)];
        if (param === constants.VENDOR) return c.vendor || 'Mozilla';
        if (param === constants.RENDERER) return c.renderer || 'ANGLE (NVIDIA GeForce RTX 3060 Direct3D11 vs_5_0 ps_5_0)';
        if (param === constants.UNMASKED_VENDOR_WEBGL) return c.unmaskedVendor || c.vendor || 'NVIDIA Corporation';
        if (param === constants.UNMASKED_RENDERER_WEBGL) return c.unmaskedRenderer || c.renderer || 'NVIDIA GeForce RTX 3060/PCIe/SSE2';
        return originalValue;
    }
    function hookProto(proto, label) {
        if (!proto || !proto.getParameter) return;
        var originalGetParameter = proto.getParameter;
        var originalGetExtension = proto.getExtension;
        var originalGetSupportedExtensions = proto.getSupportedExtensions;
        originals.push({ proto: proto, getParameter: originalGetParameter, getExtension: originalGetExtension, getSupportedExtensions: originalGetSupportedExtensions });
        proto.getParameter = function(param) {
            var originalValue;
            try { originalValue = originalGetParameter.apply(this, arguments); } catch (e) { throw e; }
            var out = spoofedParameter(param, originalValue);
            if (out !== originalValue || param === constants.VENDOR || param === constants.RENDERER || param === constants.UNMASKED_VENDOR_WEBGL || param === constants.UNMASKED_RENDERER_WEBGL) {
                push({ method: label + '.getParameter', param: Number(param), value: preview(out) });
            }
            return out;
        };
        proto.getParameter.toString = function() { return 'function getParameter() { [native code] }'; };
        if (originalGetExtension) {
            proto.getExtension = function(name) {
                var ext = originalGetExtension.apply(this, arguments);
                if (String(name).toLowerCase() === 'webgl_debug_renderer_info') {
                    push({ method: label + '.getExtension', name: String(name), supplied: !!ext });
                    return ext || { UNMASKED_VENDOR_WEBGL: constants.UNMASKED_VENDOR_WEBGL, UNMASKED_RENDERER_WEBGL: constants.UNMASKED_RENDERER_WEBGL };
                }
                return ext;
            };
            proto.getExtension.toString = function() { return 'function getExtension() { [native code] }'; };
        }
        if (originalGetSupportedExtensions) {
            proto.getSupportedExtensions = function() {
                var out = originalGetSupportedExtensions.apply(this, arguments) || [];
                if (out.indexOf('WEBGL_debug_renderer_info') === -1) out = out.concat(['WEBGL_debug_renderer_info']);
                push({ method: label + '.getSupportedExtensions', count: out.length });
                return out;
            };
            proto.getSupportedExtensions.toString = function() { return 'function getSupportedExtensions() { [native code] }'; };
        }
    }
    try { hookProto(window.WebGLRenderingContext && WebGLRenderingContext.prototype, 'WebGLRenderingContext'); } catch (e) {}
    try { hookProto(window.WebGL2RenderingContext && WebGL2RenderingContext.prototype, 'WebGL2RenderingContext'); } catch (e) {}
    window.__aida_webgl_spoof_uninstall = function() {
        var restored = [];
        for (var i = 0; i < originals.length; i++) {
            var item = originals[i];
            try { item.proto.getParameter = item.getParameter; restored.push('getParameter'); } catch (e) {}
            try { if (item.getExtension) item.proto.getExtension = item.getExtension; restored.push('getExtension'); } catch (e) {}
            try { if (item.getSupportedExtensions) item.proto.getSupportedExtensions = item.getSupportedExtensions; restored.push('getSupportedExtensions'); } catch (e) {}
        }
        window.__aida_webgl_spoof_installed = false;
        window.__aida_webgl_spoof_uninstalled = true;
        return { restored: restored };
    };
    window.__aida_webgl_spoof_installed = true;
    window.__aida_webgl_spoof_uninstalled = false;
})();
