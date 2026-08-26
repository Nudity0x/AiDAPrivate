(function() {
    if (window.__aida_event_target_installed) return;
    if (!window.EventTarget || !EventTarget.prototype) return;
    var log = window.__aida_event_target_log = window.__aida_event_target_log || [];
    var maxLog = 1200;
    var originals = {
        addEventListener: EventTarget.prototype.addEventListener,
        removeEventListener: EventTarget.prototype.removeEventListener,
        dispatchEvent: EventTarget.prototype.dispatchEvent
    };
    function stack() {
        try { return (new Error().stack || '').split('\n').slice(2, 8).join('\n'); } catch (e) { return ''; }
    }
    function targetName(target) {
        try {
            if (target === window) return 'window';
            if (target === document) return 'document';
            if (target && target.nodeName) {
                var id = target.id ? '#' + target.id : '';
                var cls = target.className && typeof target.className === 'string' ? '.' + target.className.trim().split(/\s+/).slice(0, 3).join('.') : '';
                return target.nodeName.toLowerCase() + id + cls;
            }
            return target && target.constructor ? target.constructor.name : typeof target;
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
    EventTarget.prototype.addEventListener = function(type, listener, options) {
        push({
            method: 'addEventListener',
            type: String(type),
            target: targetName(this),
            listener_name: listener && listener.name ? String(listener.name).slice(0, 120) : '',
            capture: !!(options === true || options && options.capture),
            once: !!(options && options.once),
            passive: !!(options && options.passive)
        });
        return originals.addEventListener.apply(this, arguments);
    };
    EventTarget.prototype.removeEventListener = function(type, listener, options) {
        push({
            method: 'removeEventListener',
            type: String(type),
            target: targetName(this),
            listener_name: listener && listener.name ? String(listener.name).slice(0, 120) : '',
            capture: !!(options === true || options && options.capture)
        });
        return originals.removeEventListener.apply(this, arguments);
    };
    EventTarget.prototype.dispatchEvent = function(event) {
        push({
            method: 'dispatchEvent',
            type: event && event.type ? String(event.type) : '',
            target: targetName(this),
            isTrusted: event ? !!event.isTrusted : false
        });
        return originals.dispatchEvent.apply(this, arguments);
    };
    EventTarget.prototype.addEventListener.toString = function() { return 'function addEventListener() { [native code] }'; };
    EventTarget.prototype.removeEventListener.toString = function() { return 'function removeEventListener() { [native code] }'; };
    EventTarget.prototype.dispatchEvent.toString = function() { return 'function dispatchEvent() { [native code] }'; };
    window.__aida_event_target_uninstall = function() {
        try { EventTarget.prototype.addEventListener = originals.addEventListener; } catch (e) {}
        try { EventTarget.prototype.removeEventListener = originals.removeEventListener; } catch (e) {}
        try { EventTarget.prototype.dispatchEvent = originals.dispatchEvent; } catch (e) {}
        window.__aida_event_target_installed = false;
        window.__aida_event_target_uninstalled = true;
        return { restored: ['EventTarget.prototype.addEventListener', 'EventTarget.prototype.removeEventListener', 'EventTarget.prototype.dispatchEvent'] };
    };
    window.__aida_event_target_installed = true;
    window.__aida_event_target_uninstalled = false;
})();
