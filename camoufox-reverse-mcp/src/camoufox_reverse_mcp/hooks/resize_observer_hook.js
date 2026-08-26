(function() {
    if (window.__aida_resize_observer_installed) return;
    if (!window.ResizeObserver) return;
    var Original = window.ResizeObserver;
    var log = window.__aida_resize_observer_log = window.__aida_resize_observer_log || [];
    var maxLog = 500;
    var nextId = 1;
    var ids = new WeakMap();
    function idFor(obj) {
        if (!ids.has(obj)) ids.set(obj, nextId++);
        return ids.get(obj);
    }
    function stack() {
        try { return (new Error().stack || '').split('\n').slice(2, 8).join('\n'); } catch (e) { return ''; }
    }
    function label(node) {
        try {
            if (!node) return '';
            var id = node.id ? '#' + node.id : '';
            var cls = node.className && typeof node.className === 'string' ? '.' + node.className.trim().split(/\s+/).slice(0, 3).join('.') : '';
            return String(node.nodeName || node.constructor && node.constructor.name || typeof node).toLowerCase() + id + cls;
        } catch (e) {
            return '';
        }
    }
    function sizeOf(entry) {
        try {
            var rect = entry.contentRect;
            return { width: rect && rect.width || null, height: rect && rect.height || null, target: label(entry.target) };
        } catch (e) {
            return {};
        }
    }
    function push(entry) {
        if (log.length >= maxLog) log.shift();
        entry.timestamp = Date.now();
        entry.stack = stack();
        log.push(entry);
    }
    var Wrapped = function(callback) {
        var observer;
        var wrapped = function(entries, obs) {
            var items = [];
            for (var i = 0; i < Math.min(entries.length, 20); i++) items.push(sizeOf(entries[i]));
            push({ method: 'callback', observer_id: idFor(observer || obs), entry_count: entries.length, entries: items });
            return callback.apply(this, arguments);
        };
        observer = new Original(wrapped);
        idFor(observer);
        push({ method: 'constructor', observer_id: idFor(observer) });
        return observer;
    };
    Wrapped.prototype = Original.prototype;
    try {
        var observe = Original.prototype.observe;
        var unobserve = Original.prototype.unobserve;
        var disconnect = Original.prototype.disconnect;
        Original.prototype.observe = function(target, options) {
            push({ method: 'observe', observer_id: idFor(this), target: label(target), box: options && options.box || '' });
            return observe.apply(this, arguments);
        };
        Original.prototype.unobserve = function(target) {
            push({ method: 'unobserve', observer_id: idFor(this), target: label(target) });
            return unobserve.apply(this, arguments);
        };
        Original.prototype.disconnect = function() {
            push({ method: 'disconnect', observer_id: idFor(this) });
            return disconnect.apply(this, arguments);
        };
        window.__aida_resize_observer_restore_proto = function() {
            Original.prototype.observe = observe;
            Original.prototype.unobserve = unobserve;
            Original.prototype.disconnect = disconnect;
        };
    } catch (e) {}
    window.ResizeObserver = Wrapped;
    window.__aida_resize_observer_uninstall = function() {
        try { window.ResizeObserver = Original; } catch (e) {}
        try { if (window.__aida_resize_observer_restore_proto) window.__aida_resize_observer_restore_proto(); } catch (e) {}
        window.__aida_resize_observer_installed = false;
        window.__aida_resize_observer_uninstalled = true;
        return { restored: ['ResizeObserver'] };
    };
    window.__aida_resize_observer_installed = true;
    window.__aida_resize_observer_uninstalled = false;
})();
