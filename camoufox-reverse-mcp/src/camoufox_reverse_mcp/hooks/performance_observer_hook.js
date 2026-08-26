(function() {
    if (window.__aida_performance_observer_installed) return;
    if (!window.PerformanceObserver) return;
    var Original = window.PerformanceObserver;
    var log = window.__aida_performance_observer_log = window.__aida_performance_observer_log || [];
    var maxLog = 700;
    var nextId = 1;
    var ids = new WeakMap();
    function idFor(obj) {
        if (!ids.has(obj)) ids.set(obj, nextId++);
        return ids.get(obj);
    }
    function stack() {
        try { return (new Error().stack || '').split('\n').slice(2, 8).join('\n'); } catch (e) { return ''; }
    }
    function push(entry) {
        if (log.length >= maxLog) log.shift();
        entry.timestamp = Date.now();
        entry.stack = stack();
        log.push(entry);
    }
    function summarize(entries) {
        var out = [];
        for (var i = 0; i < Math.min(entries.length, 30); i++) {
            var e = entries[i];
            out.push({
                name: String(e.name || '').slice(0, 240),
                entryType: e.entryType || '',
                startTime: e.startTime,
                duration: e.duration,
                initiatorType: e.initiatorType || ''
            });
        }
        return out;
    }
    var Wrapped = function(callback) {
        var observer;
        var wrapped = function(list, obs) {
            var entries = [];
            try { entries = list.getEntries(); } catch (e) {}
            push({ method: 'callback', observer_id: idFor(observer || obs), entry_count: entries.length, entries: summarize(entries) });
            return callback.apply(this, arguments);
        };
        observer = new Original(wrapped);
        idFor(observer);
        push({ method: 'constructor', observer_id: idFor(observer) });
        return observer;
    };
    try {
        Object.getOwnPropertyNames(Original).forEach(function(name) {
            try { Wrapped[name] = Original[name]; } catch (e) {}
        });
    } catch (e) {}
    Wrapped.prototype = Original.prototype;
    try {
        var observe = Original.prototype.observe;
        var disconnect = Original.prototype.disconnect;
        var takeRecords = Original.prototype.takeRecords;
        Original.prototype.observe = function(options) {
            push({ method: 'observe', observer_id: idFor(this), options: JSON.parse(JSON.stringify(options || {})) });
            return observe.apply(this, arguments);
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
        window.__aida_performance_observer_restore_proto = function() {
            Original.prototype.observe = observe;
            Original.prototype.disconnect = disconnect;
            Original.prototype.takeRecords = takeRecords;
        };
    } catch (e) {}
    window.PerformanceObserver = Wrapped;
    window.__aida_performance_observer_uninstall = function() {
        try { window.PerformanceObserver = Original; } catch (e) {}
        try { if (window.__aida_performance_observer_restore_proto) window.__aida_performance_observer_restore_proto(); } catch (e) {}
        window.__aida_performance_observer_installed = false;
        window.__aida_performance_observer_uninstalled = true;
        return { restored: ['PerformanceObserver'] };
    };
    window.__aida_performance_observer_installed = true;
    window.__aida_performance_observer_uninstalled = false;
})();
