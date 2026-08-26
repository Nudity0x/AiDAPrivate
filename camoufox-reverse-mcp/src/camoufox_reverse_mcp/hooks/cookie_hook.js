(function () {
    var state = window.__mcp_cookie_hook_state || {};
    if (window.__mcp_cookie_hook_installed && state.installed) {
        state.reinstallAttempts = (state.reinstallAttempts || 0) + 1;
        state.lastReinstallTs = Date.now();
        window.__mcp_cookie_hook_state = state;
        return;
    }
    window.__mcp_cookie_hook_installed = true;
    window.__mcp_cookie_log = Array.isArray(window.__mcp_cookie_log) ? window.__mcp_cookie_log : [];
    state = {
        installed: false,
        installedAt: Date.now(),
        ownerName: '',
        prototypeInstalled: false,
        documentInstalled: false,
        setterCalls: 0,
        getterCalls: 0,
        logAppends: 0,
        lastError: '',
        lastSetName: '',
        lastValueLength: 0
    };
    window.__mcp_cookie_hook_state = state;

    function cookieName(value) {
        var text = String(value || '');
        var idx = text.indexOf('=');
        return idx > 0 ? text.slice(0, idx).trim() : '';
    }

    function append(op, value) {
        var text = String(value == null ? '' : value);
        var entry = {
            op: op,
            value: text,
            stack: '',
            ts: Date.now(),
            href: String(location && location.href || '')
        };
        try {
            entry.stack = String(new Error().stack || '');
        } catch (e) {}
        try {
            window.__mcp_cookie_log.push(entry);
            while (window.__mcp_cookie_log.length > 1000) window.__mcp_cookie_log.shift();
            state.logAppends++;
            state.lastValueLength = text.length;
            if (op === 'set') state.lastSetName = cookieName(text);
            console.log('[COOKIE-HOOK] append op=' + op + ' name=' + (state.lastSetName || '') + ' count=' + window.__mcp_cookie_log.length);
        } catch (e) {
            state.lastError = 'append:' + String(e && (e.message || e));
        }
    }

    function findCookieDescriptor() {
        var proto = document;
        while (proto) {
            var descriptor = Object.getOwnPropertyDescriptor(proto, 'cookie');
            if (descriptor && descriptor.get && descriptor.set) return { descriptor: descriptor, owner: proto };
            proto = Object.getPrototypeOf(proto);
        }
        return null;
    }

    var found = findCookieDescriptor();
    if (!found) {
        state.lastError = 'descriptor_not_found';
        console.warn('[COOKIE-HOOK] descriptor_not_found');
        return;
    }

    var descriptor = found.descriptor;
    var owner = found.owner;
    var originalSet = descriptor.set;
    var originalGet = descriptor.get;
    var ownerName = '';
    try {
        ownerName = owner && owner.constructor ? String(owner.constructor.name || '') : '';
    } catch (e) {}
    state.ownerName = ownerName || 'Document';

    function makeDescriptor(label) {
        return {
            set: function(value) {
                state.setterCalls++;
                append('set', value);
                return originalSet.call(this, value);
            },
            get: function() {
                var value = originalGet.call(this);
                state.getterCalls++;
                append('get', value);
                return value;
            },
            configurable: true,
            enumerable: descriptor.enumerable !== false
        };
    }

    try {
        Object.defineProperty(owner, 'cookie', makeDescriptor('prototype'));
        state.prototypeInstalled = true;
    } catch (e) {
        state.lastError = 'prototype:' + String(e && (e.message || e));
    }

    try {
        Object.defineProperty(document, 'cookie', makeDescriptor('document'));
        state.documentInstalled = true;
    } catch (e) {
        state.lastError = (state.lastError ? state.lastError + '|' : '') + 'document:' + String(e && (e.message || e));
    }

    state.installed = state.prototypeInstalled || state.documentInstalled;
    window.__mcp_cookie_hook_state = state;
    console.log('[COOKIE-HOOK] installed owner=' + state.ownerName + ' prototype=' + (state.prototypeInstalled ? 1 : 0) + ' document=' + (state.documentInstalled ? 1 : 0));
})();
