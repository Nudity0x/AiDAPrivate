(function() {
    if (window.__aida_intersection_observer_installed) return;
    if (!window.IntersectionObserver) return;
    var Original = window.IntersectionObserver;
    var log = window.__aida_intersection_observer_log = window.__aida_intersection_observer_log || [];
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
            if (node === document) return 'document';
            var id = node.id ? '#' + node.id : '';
            var cls = node.className && typeof node.className === 'string' ? '.' + node.className.trim().split(/\s+/).slice(0, 3).join('.') : '';
            return String(node.nodeName || node.constructor && node.constructor.name || typeof node).toLowerCase() + id + cls;
        } catch (e) {
            return '';
        }
    }
    function push(entry) {
        if (log.length >= maxLog) log.shift();
        entry.timestamp = Date.now();
        entry.stack = stack();
        log.push(entry);
    }
    function summarize(entries) {
        var out = [];
        for (var i = 0; i < Math.min(entries.length, 20); i++) {
            var e = entries[i];
            out.push({ target: label(e.target), isIntersecting: !!e.isIntersecting, ratio: e.intersectionRatio, time: e.time });
        }
        return out;
    }
    var Wrapped = function(callback, options) {
        var observer;
        var wrapped = function(entries, obs) {
            push({ method: 'callback', observer_id: idFor(observer || obs), entry_count: entries.length, entries: summarize(entries) });
            return callback.apply(this, arguments);
        };
        observer = new Original(wrapped, options);
        idFor(observer);
        push({ method: 'constructor', observer_id: idFor(observer), root: options && options.root ? label(options.root) : null, rootMargin: options && options.rootMargin || '', threshold: options && options.threshold });
        return observer;
    };
    Wrapped.prototype = Original.prototype;
    try {
        var observe = Original.prototype.observe;
        var unobserve = Original.prototype.unobserve;
        var disconnect = Original.prototype.disconnect;
        var takeRecords = Original.prototype.takeRecords;
        Original.prototype.observe = function(target) {
            push({ method: 'observe', observer_id: idFor(this), target: label(target) });
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
        Original.prototype.takeRecords = function() {
            var entries = takeRecords.apply(this, arguments);
            push({ method: 'takeRecords', observer_id: idFor(this), entry_count: entries.length, entries: summarize(entries) });
            return entries;
        };
        window.__aida_intersection_observer_restore_proto = function() {
            Original.prototype.observe = observe;
            Original.prototype.unobserve = unobserve;
            Original.prototype.disconnect = disconnect;
            Original.prototype.takeRecords = takeRecords;
        };
    } catch (e) {}
    window.IntersectionObserver = Wrapped;
    window.__aida_intersection_observer_uninstall = function() {
        try { window.IntersectionObserver = Original; } catch (e) {}
        try { if (window.__aida_intersection_observer_restore_proto) window.__aida_intersection_observer_restore_proto(); } catch (e) {}
        window.__aida_intersection_observer_installed = false;
        window.__aida_intersection_observer_uninstalled = true;
        return { restored: ['IntersectionObserver'] };
    };
    window.__aida_intersection_observer_installed = true;
    window.__aida_intersection_observer_uninstalled = false;
})();
