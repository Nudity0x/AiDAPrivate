(function() {
    window.__mcp_fetch_log = window.__mcp_fetch_log || [];
    window.__mcp_fetch_initiator_log = window.__mcp_fetch_initiator_log || [];

    const currentFetch = window.fetch;
    if (window.__mcp_fetch_hooked && currentFetch && currentFetch.__mcp_fetch_wrapper === true) return;
    const _fetch = currentFetch && currentFetch.__mcp_fetch_original ? currentFetch.__mcp_fetch_original : currentFetch;
    if (typeof _fetch !== 'function') {
        window.__mcp_fetch_hook_error = 'fetch is not callable';
        return;
    }
    const _apply = Reflect.apply;

    const hookedFetch = async function(input, init) {
        init = init || {};
        const url = typeof input === 'string' ? input : (input instanceof Request ? input.url : String(input));
        const method = init.method || (input instanceof Request ? input.method : 'GET') || 'GET';
        const info = {
            url, method,
            headers: init.headers ? (typeof init.headers === 'object' ? Object.assign({}, init.headers) : {}) : {},
            body: init.body ? String(init.body).substring(0, 5000) : null,
            stack: new Error().stack,
            timestamp: Date.now()
        };

        try {
            var _stack = '';
            try { _stack = new Error().stack || ''; } catch (e) {}
            window.__mcp_fetch_initiator_log.push({
                url: String(url),
                method: method,
                stack: _stack.split('\n').slice(0, 15).join('\n'),
                ts: Date.now()
            });
            if (window.__mcp_fetch_initiator_log.length > 500) {
                window.__mcp_fetch_initiator_log.shift();
            }
        } catch (e) {}

        try {
            const response = await _apply(_fetch, this, arguments);
            info.status = response.status;
            info.ok = response.ok;
            window.__mcp_fetch_log.push(info);
            if (window.__mcp_fetch_log.length > 500) window.__mcp_fetch_log.shift();
            return response;
        } catch (e) {
            info.error = e.message;
            window.__mcp_fetch_log.push(info);
            throw e;
        }
    };

    hookedFetch.toString = function() { return 'function fetch() { [native code] }'; };
    try {
        Object.defineProperty(hookedFetch, '__mcp_fetch_wrapper', {
            value: true, writable: false, configurable: false
        });
        Object.defineProperty(hookedFetch, '__mcp_fetch_original', {
            value: _fetch, writable: false, configurable: false
        });
    } catch(e) {
        hookedFetch.__mcp_fetch_wrapper = true;
        hookedFetch.__mcp_fetch_original = _fetch;
    }

    try {
        Object.defineProperty(window, 'fetch', {
            value: hookedFetch, writable: false, configurable: false
        });
    } catch(e) {
        window.fetch = hookedFetch;
    }
    window.__mcp_fetch_hooked = window.fetch && window.fetch.__mcp_fetch_wrapper === true;
})();
