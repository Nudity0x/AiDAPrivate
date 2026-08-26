(function() {
    if (window.__aida_crypto_subtle_installed) return;
    if (!window.crypto || !crypto.subtle) return;
    var log = window.__aida_crypto_subtle_log = window.__aida_crypto_subtle_log || [];
    var maxLog = 700;
    var originals = [];
    var methods = ['digest', 'encrypt', 'decrypt', 'sign', 'verify', 'deriveKey', 'deriveBits', 'importKey', 'exportKey', 'wrapKey', 'unwrapKey', 'generateKey'];
    function stack() {
        try { return (new Error().stack || '').split('\n').slice(2, 8).join('\n'); } catch (e) { return ''; }
    }
    function push(entry) {
        if (log.length >= maxLog) log.shift();
        entry.timestamp = Date.now();
        entry.stack = stack();
        log.push(entry);
    }
    function byteLength(value) {
        try {
            if (value == null) return 0;
            if (typeof value === 'string') return value.length;
            if (value instanceof ArrayBuffer) return value.byteLength;
            if (ArrayBuffer.isView(value)) return value.byteLength;
            if (value.buffer && value.byteLength != null) return value.byteLength;
            if (value.length != null) return Number(value.length) || 0;
        } catch (e) {}
        return null;
    }
    function algorithm(value) {
        try {
            if (typeof value === 'string') return value;
            if (value && value.name) return String(value.name);
            if (value && value.hash) {
                var hash = typeof value.hash === 'string' ? value.hash : value.hash.name;
                return String(value.name || '') + ':' + String(hash || '');
            }
        } catch (e) {}
        return '';
    }
    function keyInfo(value) {
        try {
            if (!value || !value.type && !value.algorithm && !value.usages) return null;
            return {
                type: value.type || '',
                extractable: value.extractable == null ? null : !!value.extractable,
                algorithm: algorithm(value.algorithm || {}),
                usages: Array.isArray(value.usages) ? value.usages.slice(0, 12) : []
            };
        } catch (e) {
            return null;
        }
    }
    function argsInfo(method, args) {
        var out = { method: method };
        if (args.length > 0) out.algorithm = algorithm(args[0]);
        if (method === 'digest') out.data_len = byteLength(args[1]);
        if (method === 'encrypt' || method === 'decrypt' || method === 'sign' || method === 'verify') {
            out.key = keyInfo(args[1]);
            out.data_len = byteLength(method === 'verify' ? args[3] : args[2]);
            if (method === 'verify') out.signature_len = byteLength(args[2]);
        }
        if (method === 'deriveKey' || method === 'deriveBits') out.base_key = keyInfo(args[1]);
        if (method === 'importKey') {
            out.format = String(args[0] || '');
            out.key_data_len = byteLength(args[1]);
            out.extractable = !!args[3];
            out.usages = Array.isArray(args[4]) ? args[4].slice(0, 12) : [];
        }
        if (method === 'exportKey') {
            out.format = String(args[0] || '');
            out.key = keyInfo(args[1]);
        }
        if (method === 'wrapKey') {
            out.format = String(args[0] || '');
            out.key = keyInfo(args[1]);
            out.wrapping_key = keyInfo(args[2]);
            out.wrap_algorithm = algorithm(args[3]);
        }
        if (method === 'unwrapKey') {
            out.format = String(args[0] || '');
            out.wrapped_key_len = byteLength(args[1]);
            out.unwrapping_key = keyInfo(args[2]);
            out.unwrap_algorithm = algorithm(args[3]);
            out.unwrapped_algorithm = algorithm(args[4]);
            out.extractable = !!args[5];
            out.usages = Array.isArray(args[6]) ? args[6].slice(0, 12) : [];
        }
        if (method === 'generateKey') {
            out.extractable = !!args[1];
            out.usages = Array.isArray(args[2]) ? args[2].slice(0, 12) : [];
        }
        return out;
    }
    function resultInfo(result) {
        try {
            if (result instanceof ArrayBuffer || ArrayBuffer.isView(result)) return { result_len: byteLength(result), result_type: Object.prototype.toString.call(result) };
            if (result && result.publicKey && result.privateKey) return { result_type: 'CryptoKeyPair', publicKey: keyInfo(result.publicKey), privateKey: keyInfo(result.privateKey) };
            var ki = keyInfo(result);
            if (ki) return { result_type: 'CryptoKey', key: ki };
        } catch (e) {}
        return { result_type: result == null ? String(result) : Object.prototype.toString.call(result) };
    }
    function hookMethod(method) {
        var owner = Object.prototype.hasOwnProperty.call(crypto.subtle, method) ? crypto.subtle : Object.getPrototypeOf(crypto.subtle);
        var original = owner && owner[method];
        if (typeof original !== 'function') return;
        originals.push({ method: method, original: original, owner: owner });
        owner[method] = function() {
            var args = Array.prototype.slice.call(arguments);
            var entry = argsInfo(method, args);
            entry.phase = 'call';
            push(entry);
            try {
                var promise = original.apply(this, arguments);
                if (promise && typeof promise.then === 'function') {
                    return promise.then(function(result) {
                        var done = argsInfo(method, args);
                        var ri = resultInfo(result);
                        Object.keys(ri).forEach(function(k) { done[k] = ri[k]; });
                        done.phase = 'resolve';
                        push(done);
                        return result;
                    }, function(err) {
                        var fail = argsInfo(method, args);
                        fail.phase = 'reject';
                        fail.error_name = err && err.name || '';
                        fail.error_message = err && err.message ? String(err.message).slice(0, 200) : '';
                        push(fail);
                        throw err;
                    });
                }
                return promise;
            } catch (e) {
                var thrown = argsInfo(method, args);
                thrown.phase = 'throw';
                thrown.error_name = e && e.name || '';
                thrown.error_message = e && e.message ? String(e.message).slice(0, 200) : '';
                push(thrown);
                throw e;
            }
        };
        owner[method].toString = function() { return 'function ' + method + '() { [native code] }'; };
    }
    for (var i = 0; i < methods.length; i++) {
        try { hookMethod(methods[i]); } catch (e) {}
    }
    window.__aida_crypto_subtle_uninstall = function() {
        var restored = [];
        for (var i = 0; i < originals.length; i++) {
            try {
                originals[i].owner[originals[i].method] = originals[i].original;
                restored.push('crypto.subtle.' + originals[i].method);
            } catch (e) {}
        }
        window.__aida_crypto_subtle_installed = false;
        window.__aida_crypto_subtle_uninstalled = true;
        return { restored: restored };
    };
    window.__aida_crypto_subtle_installed = true;
    window.__aida_crypto_subtle_uninstalled = false;
})();
