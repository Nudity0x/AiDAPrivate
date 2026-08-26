(function() {
    if (window.__aida_mutation_observer_installed) return;
    var Original = window.MutationObserver || window.WebKitMutationObserver;
    if (!Original) return;
    var log = window.__aida_mutation_observer_log = window.__aida_mutation_observer_log || [];
    var maxLog = 600;
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
            if (node === window) return 'window';
            var name = node.nodeName || node.constructor && node.constructor.name || typeof node;
            var id = node.id ? '#' + node.id : '';
            var cls = node.className && typeof node.className === 'string' ? '.' + node.className.trim().split(/\s+/).slice(0, 3).join('.') : '';
            return String(name).toLowerCase() + id + cls;
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
    function summarize(records) {
        var out = [];
        for (var i = 0; i < Math.min(records.length, 20); i++) {
            var r = records[i];
            out.push({
                type: r.type,
                target: label(r.target),
                attributeName: r.attributeName || null,
                addedNodes: r.addedNodes ? r.addedNodes.length : 0,
                removedNodes: r.removedNodes ? r.removedNodes.length : 0
            });
        }
        return out;
    }
    var Wrapped = function(callback) {
        var observer;
        var wrappedCallback = function(records, obs) {
            push({ method: 'callback', observer_id: idFor(observer || obs), record_count: records.length, records: summarize(records) });
            return callback.apply(this, arguments);
        };
        observer = new Original(wrappedCallback);
        idFor(observer);
        push({ method: 'constructor', observer_id: idFor(observer) });
        return observer;
    };
    Wrapped.prototype = Original.prototype;
    try {
        var observe = Original.prototype.observe;
        var disconnect = Original.prototype.disconnect;
        var takeRecords = Original.prototype.takeRecords;
        Original.prototype.observe = function(target, options) {
            push({ method: 'observe', observer_id: idFor(this), target: label(target), options: JSON.parse(JSON.stringify(options || {})) });
            return observe.apply(this, arguments);
        };
        Original.prototype.disconnect = function() {
            push({ method: 'disconnect', observer_id: idFor(this) });
            return disconnect.apply(this, arguments);
        };
        Original.prototype.takeRecords = function() {
            var records = takeRecords.apply(this, arguments);
            push({ method: 'takeRecords', observer_id: idFor(this), record_count: records.length, records: summarize(records) });
            return records;
        };
        window.__aida_mutation_observer_restore_proto = function() {
            Original.prototype.observe = observe;
            Original.prototype.disconnect = disconnect;
            Original.prototype.takeRecords = takeRecords;
        };
    } catch (e) {}
    window.MutationObserver = Wrapped;
    try { if (window.WebKitMutationObserver) window.WebKitMutationObserver = Wrapped; } catch (e) {}
    window.__aida_mutation_observer_uninstall = function() {
        try { window.MutationObserver = Original; } catch (e) {}
        try { if (window.WebKitMutationObserver) window.WebKitMutationObserver = Original; } catch (e) {}
        try { if (window.__aida_mutation_observer_restore_proto) window.__aida_mutation_observer_restore_proto(); } catch (e) {}
        window.__aida_mutation_observer_installed = false;
        window.__aida_mutation_observer_uninstalled = true;
        return { restored: ['MutationObserver'] };
    };
    window.__aida_mutation_observer_installed = true;
    window.__aida_mutation_observer_uninstalled = false;
})();
