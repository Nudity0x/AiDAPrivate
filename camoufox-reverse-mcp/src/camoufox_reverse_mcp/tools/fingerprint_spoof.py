from __future__ import annotations

import hashlib
import json
import os
import secrets
import time
from typing import Any

from ..browser import _camoufox_debug
from ..server import mcp, browser_manager


_SESSION_SEED = secrets.token_hex(16)
_VALID_ACTIONS = {
    "canvas_spoof",
    "webgl_spoof",
    "audio_spoof",
    "font_spoof",
    "timezone_spoof",
    "geolocation_spoof",
    "screen_viewport_spoof",
    "battery_spoof",
    "sensors_spoof",
    "navigator_spoof",
    "verify",
}


def _hooks_dir() -> str:
    return os.path.join(os.path.dirname(os.path.dirname(__file__)), "hooks")


def _read_hook(filename: str) -> str:
    with open(os.path.join(_hooks_dir(), filename), "r", encoding="utf-8") as handle:
        return handle.read()


def _seed_for(action: str, page_id: str | None, custom: dict[str, Any] | None = None) -> int:
    material = {
        "session": _SESSION_SEED,
        "action": action,
        "page_id": page_id or "default",
        "custom": custom or {},
    }
    digest = hashlib.sha256(json.dumps(material, sort_keys=True, default=str).encode("utf-8")).digest()
    return int.from_bytes(digest[:4], "big") or 1


def _bounded_float(value: object, fallback: float, minimum: float, maximum: float) -> float:
    try:
        parsed = float(value)
    except Exception:
        return fallback
    if parsed < minimum:
        return minimum
    if parsed > maximum:
        return maximum
    return parsed


def _clean_text(value: object, fallback: str = "", limit: int = 240) -> str:
    text = str(value if value is not None else fallback).strip()
    if len(text) > limit:
        return text[:limit]
    return text


def _clean_list(values: list[str] | None, limit: int = 80) -> list[str]:
    if not values:
        return []
    result: list[str] = []
    for value in values[:limit]:
        text = _clean_text(value, "", 120)
        if text:
            result.append(text)
    return result


def _script_with_config(global_name: str, config: dict[str, Any], hook_file: str) -> str:
    return (
        f"window.{global_name} = {json.dumps(config, separators=(',', ':'), sort_keys=True)};\n"
        + _read_hook(hook_file)
    )


async def _install_script(name: str, script: str, persistent: bool, page_id: str | None, operation: str, aida_operation_id=None) -> dict:
    page = await browser_manager.resolve_page_for_operation(page_id, operation, True, aida_operation_id)
    resolved_page_id = browser_manager.page_id_for(page) or page_id or ""
    warning = None
    if persistent:
        await browser_manager.add_persistent_script(name, script)
    else:
        await page.add_init_script(script=script)
    try:
        await page.evaluate(script)
    except Exception as exc:
        warning = str(exc)[:400]
    if name not in browser_manager._init_scripts:
        browser_manager._init_scripts.append(name)
    result = {
        "status": "installed" if warning is None else "installed_with_warning",
        "name": name,
        "persistent": bool(persistent),
        "page_id": resolved_page_id,
        "applied_to_current_page": warning is None,
        "script_sha256": hashlib.sha256(script.encode("utf-8")).hexdigest()[:16],
    }
    if warning:
        result["warning"] = warning
    return result


def _canvas_script(mode: str, noise_level: float, seed: int, custom: dict[str, Any] | None) -> str:
    config = {
        "mode": mode,
        "noiseLevel": noise_level,
        "seed": seed,
        "custom": custom or {},
    }
    return _script_with_config("__aida_canvas_spoof_config", config, "canvas_spoof.js")


def _webgl_script(
    renderer: str | None,
    vendor: str | None,
    unmasked_renderer: str | None,
    unmasked_vendor: str | None,
    seed: int,
    custom: dict[str, Any] | None,
) -> str:
    config = {
        "renderer": _clean_text(renderer, "ANGLE (NVIDIA GeForce RTX 3060 Direct3D11 vs_5_0 ps_5_0)", 180),
        "vendor": _clean_text(vendor, "Mozilla", 120),
        "unmaskedRenderer": _clean_text(unmasked_renderer, renderer or "NVIDIA GeForce RTX 3060/PCIe/SSE2", 180),
        "unmaskedVendor": _clean_text(unmasked_vendor, vendor or "NVIDIA Corporation", 120),
        "seed": seed,
        "custom": custom or {},
    }
    return _script_with_config("__aida_webgl_spoof_config", config, "webgl_spoof.js")


def _audio_script(mode: str, noise_level: float, seed: int, custom: dict[str, Any] | None) -> str:
    config = {
        "mode": mode,
        "noiseLevel": noise_level,
        "seed": seed,
        "custom": custom or {},
    }
    return _script_with_config("__aida_audio_spoof_config", config, "audio_spoof.js")


def _font_script(block_fonts: list[str] | None, allow_fonts: list[str] | None, seed: int, custom: dict[str, Any] | None) -> str:
    config = {
        "blockFonts": _clean_list(block_fonts),
        "allowFonts": _clean_list(allow_fonts),
        "seed": seed,
        "custom": custom or {},
    }
    return _script_with_config("__aida_font_spoof_config", config, "font_fallback.js")


def _timezone_script(timezone: str) -> str:
    config = {"timezone": timezone}
    return f"""window.__aida_timezone_spoof_config = {json.dumps(config, separators=(',', ':'), sort_keys=True)};
(function() {{
    if (window.__aida_timezone_spoof_installed) {{
        return;
    }}
    var cfg = window.__aida_timezone_spoof_config || {{}};
    var log = window.__aida_timezone_spoof_log = window.__aida_timezone_spoof_log || [];
    var originals = window.__aida_timezone_spoof_originals = {{}};
    var maxLog = 120;
    function push(entry) {{
        if (log.length >= maxLog) log.shift();
        entry.timestamp = Date.now();
        try {{ entry.stack = (new Error().stack || '').split('\\n').slice(2, 7).join('\\n'); }} catch (e) {{}}
        log.push(entry);
    }}
    function targetOffset(date) {{
        var tz = String((window.__aida_timezone_spoof_config || cfg).timezone || 'UTC');
        try {{
            var parts = new Intl.DateTimeFormat('en-US', {{
                timeZone: tz,
                year: 'numeric',
                month: '2-digit',
                day: '2-digit',
                hour: '2-digit',
                minute: '2-digit',
                second: '2-digit',
                hourCycle: 'h23'
            }}).formatToParts(date);
            var bag = {{}};
            for (var i = 0; i < parts.length; i++) bag[parts[i].type] = parts[i].value;
            var asUtc = Date.UTC(+bag.year, +bag.month - 1, +bag.day, +bag.hour, +bag.minute, +bag.second);
            return Math.round((date.getTime() - asUtc) / 60000);
        }} catch (e) {{
            return 0;
        }}
    }}
    try {{
        originals.resolvedOptions = Intl.DateTimeFormat.prototype.resolvedOptions;
        Intl.DateTimeFormat.prototype.resolvedOptions = function() {{
            var out = originals.resolvedOptions.apply(this, arguments);
            try {{ out.timeZone = String((window.__aida_timezone_spoof_config || cfg).timezone || out.timeZone || 'UTC'); }} catch (e) {{}}
            push({{ method: 'Intl.DateTimeFormat.resolvedOptions', timezone: out.timeZone }});
            return out;
        }};
        Intl.DateTimeFormat.prototype.resolvedOptions.toString = function() {{ return 'function resolvedOptions() {{ [native code] }}'; }};
    }} catch (e) {{}}
    try {{
        originals.getTimezoneOffset = Date.prototype.getTimezoneOffset;
        Date.prototype.getTimezoneOffset = function() {{
            var offset = targetOffset(this);
            push({{ method: 'Date.getTimezoneOffset', offset: offset }});
            return offset;
        }};
        Date.prototype.getTimezoneOffset.toString = function() {{ return 'function getTimezoneOffset() {{ [native code] }}'; }};
    }} catch (e) {{}}
    window.__aida_timezone_spoof_uninstall = function() {{
        try {{ if (originals.resolvedOptions) Intl.DateTimeFormat.prototype.resolvedOptions = originals.resolvedOptions; }} catch (e) {{}}
        try {{ if (originals.getTimezoneOffset) Date.prototype.getTimezoneOffset = originals.getTimezoneOffset; }} catch (e) {{}}
        window.__aida_timezone_spoof_installed = false;
        window.__aida_timezone_spoof_uninstalled = true;
        return {{ restored: ['Intl.DateTimeFormat.prototype.resolvedOptions', 'Date.prototype.getTimezoneOffset'] }};
    }};
    window.__aida_timezone_spoof_installed = true;
    window.__aida_timezone_spoof_uninstalled = false;
}})();"""


def _geolocation_script(latitude: float, longitude: float, accuracy: float) -> str:
    config = {"latitude": latitude, "longitude": longitude, "accuracy": accuracy}
    return f"""window.__aida_geolocation_spoof_config = {json.dumps(config, separators=(',', ':'), sort_keys=True)};
(function() {{
    if (window.__aida_geolocation_spoof_installed) {{
        return;
    }}
    var cfg = window.__aida_geolocation_spoof_config || {{}};
    var log = window.__aida_geolocation_spoof_log = window.__aida_geolocation_spoof_log || [];
    var originals = window.__aida_geolocation_spoof_originals = {{}};
    var watches = window.__aida_geolocation_spoof_watches = window.__aida_geolocation_spoof_watches || {{}};
    var nextWatch = 1;
    function push(entry) {{
        if (log.length >= 160) log.shift();
        entry.timestamp = Date.now();
        try {{ entry.stack = (new Error().stack || '').split('\\n').slice(2, 7).join('\\n'); }} catch (e) {{}}
        log.push(entry);
    }}
    function position() {{
        var current = window.__aida_geolocation_spoof_config || cfg;
        return {{
            coords: {{
                latitude: Number(current.latitude || 0),
                longitude: Number(current.longitude || 0),
                accuracy: Number(current.accuracy || 50),
                altitude: null,
                altitudeAccuracy: null,
                heading: null,
                speed: null
            }},
            timestamp: Date.now()
        }};
    }}
    function ensureGeo() {{
        if (navigator.geolocation) return navigator.geolocation;
        var geo = {{}};
        try {{
            Object.defineProperty(Navigator.prototype, 'geolocation', {{ get: function() {{ return geo; }}, configurable: true }});
        }} catch (e) {{
            try {{ navigator.geolocation = geo; }} catch (ignored) {{}}
        }}
        return geo;
    }}
    var geo = ensureGeo();
    try {{
        originals.getCurrentPosition = geo.getCurrentPosition;
        originals.watchPosition = geo.watchPosition;
        originals.clearWatch = geo.clearWatch;
        geo.getCurrentPosition = function(success, error, options) {{
            push({{ method: 'getCurrentPosition', hasSuccess: typeof success === 'function', hasError: typeof error === 'function' }});
            var pos = position();
            setTimeout(function() {{ if (typeof success === 'function') success(pos); }}, 0);
        }};
        geo.watchPosition = function(success, error, options) {{
            var id = nextWatch++;
            push({{ method: 'watchPosition', id: id, hasSuccess: typeof success === 'function', hasError: typeof error === 'function' }});
            watches[id] = setInterval(function() {{
                if (typeof success === 'function') success(position());
            }}, Math.max(1000, Number(options && options.maximumAge || 1000)));
            setTimeout(function() {{ if (typeof success === 'function') success(position()); }}, 0);
            return id;
        }};
        geo.clearWatch = function(id) {{
            push({{ method: 'clearWatch', id: id }});
            if (watches[id]) {{
                clearInterval(watches[id]);
                delete watches[id];
            }}
        }};
        geo.getCurrentPosition.toString = function() {{ return 'function getCurrentPosition() {{ [native code] }}'; }};
        geo.watchPosition.toString = function() {{ return 'function watchPosition() {{ [native code] }}'; }};
        geo.clearWatch.toString = function() {{ return 'function clearWatch() {{ [native code] }}'; }};
    }} catch (e) {{}}
    try {{
        if (navigator.permissions && typeof navigator.permissions.query === 'function') {{
            originals.permissionsQuery = navigator.permissions.query.bind(navigator.permissions);
            navigator.permissions.query = function(desc) {{
                if (desc && desc.name === 'geolocation') {{
                    push({{ method: 'permissions.query', name: 'geolocation' }});
                    return Promise.resolve({{ state: 'granted', onchange: null }});
                }}
                return originals.permissionsQuery.apply(this, arguments);
            }};
            navigator.permissions.query.toString = function() {{ return 'function query() {{ [native code] }}'; }};
        }}
    }} catch (e) {{}}
    window.__aida_geolocation_spoof_uninstall = function() {{
        try {{ if (originals.getCurrentPosition) geo.getCurrentPosition = originals.getCurrentPosition; }} catch (e) {{}}
        try {{ if (originals.watchPosition) geo.watchPosition = originals.watchPosition; }} catch (e) {{}}
        try {{ if (originals.clearWatch) geo.clearWatch = originals.clearWatch; }} catch (e) {{}}
        try {{ if (originals.permissionsQuery && navigator.permissions) navigator.permissions.query = originals.permissionsQuery; }} catch (e) {{}}
        try {{ Object.keys(watches).forEach(function(id) {{ clearInterval(watches[id]); delete watches[id]; }}); }} catch (e) {{}}
        window.__aida_geolocation_spoof_installed = false;
        window.__aida_geolocation_spoof_uninstalled = true;
        return {{ restored: ['navigator.geolocation', 'navigator.permissions.query'] }};
    }};
    window.__aida_geolocation_spoof_installed = true;
    window.__aida_geolocation_spoof_uninstalled = false;
}})();"""


def _screen_viewport_script(config: dict[str, Any]) -> str:
    cfg = {
        "width": _bounded_float(config.get("width"), 1920, 320, 10000),
        "height": _bounded_float(config.get("height"), 1080, 240, 10000),
        "availWidth": _bounded_float(config.get("avail_width", config.get("availWidth")), config.get("width", 1920), 320, 10000),
        "availHeight": _bounded_float(config.get("avail_height", config.get("availHeight")), config.get("height", 1080), 240, 10000),
        "colorDepth": int(_bounded_float(config.get("color_depth", config.get("colorDepth")), 24, 1, 64)),
        "pixelDepth": int(_bounded_float(config.get("pixel_depth", config.get("pixelDepth")), 24, 1, 64)),
        "deviceScaleFactor": _bounded_float(config.get("device_scale_factor", config.get("deviceScaleFactor")), 1.0, 0.25, 8.0),
    }
    return f"""window.__aida_screen_viewport_spoof_config = {json.dumps(cfg, separators=(',', ':'), sort_keys=True)};
(function() {{
    var cfg = window.__aida_screen_viewport_spoof_config || {{}};
    var log = window.__aida_screen_viewport_spoof_log = window.__aida_screen_viewport_spoof_log || [];
    function push(method, value) {{ if (log.length >= 120) log.shift(); log.push({{ method: method, value: value, timestamp: Date.now() }}); }}
    function define(target, key, value) {{
        try {{ Object.defineProperty(target, key, {{ get: function() {{ push(key, value); return value; }}, configurable: true }}); }} catch (e) {{}}
    }}
    var screenTarget = window.Screen && window.Screen.prototype ? window.Screen.prototype : (window.screen || {{}});
    define(screenTarget, 'width', Math.round(Number(cfg.width || 1920)));
    define(screenTarget, 'height', Math.round(Number(cfg.height || 1080)));
    define(screenTarget, 'availWidth', Math.round(Number(cfg.availWidth || cfg.width || 1920)));
    define(screenTarget, 'availHeight', Math.round(Number(cfg.availHeight || cfg.height || 1080)));
    define(screenTarget, 'colorDepth', Math.round(Number(cfg.colorDepth || 24)));
    define(screenTarget, 'pixelDepth', Math.round(Number(cfg.pixelDepth || cfg.colorDepth || 24)));
    define(window, 'devicePixelRatio', Number(cfg.deviceScaleFactor || 1));
    window.__aida_screen_viewport_spoof_installed = true;
}})();"""


def _battery_script(config: dict[str, Any]) -> str:
    cfg = {
        "charging": bool(config.get("charging", True)),
        "level": _bounded_float(config.get("level"), 0.86, 0.0, 1.0),
        "chargingTime": int(_bounded_float(config.get("charging_time", config.get("chargingTime")), 0, 0, 86400)),
        "dischargingTime": int(_bounded_float(config.get("discharging_time", config.get("dischargingTime")), 14400, 0, 86400)),
    }
    return f"""window.__aida_battery_spoof_config = {json.dumps(cfg, separators=(',', ':'), sort_keys=True)};
(function() {{
    var cfg = window.__aida_battery_spoof_config || {{}};
    var listeners = {{}};
    var battery = {{
        charging: !!cfg.charging,
        level: Number(cfg.level || 0),
        chargingTime: Number(cfg.chargingTime || 0),
        dischargingTime: Number(cfg.dischargingTime || Infinity),
        onchargingchange: null,
        onlevelchange: null,
        onchargingtimechange: null,
        ondischargingtimechange: null,
        addEventListener: function(type, cb) {{ listeners[type] = listeners[type] || []; if (typeof cb === 'function') listeners[type].push(cb); }},
        removeEventListener: function(type, cb) {{ if (!listeners[type]) return; listeners[type] = listeners[type].filter(function(x) {{ return x !== cb; }}); }},
        dispatchEvent: function(evt) {{ var type = evt && evt.type || ''; (listeners[type] || []).slice().forEach(function(cb) {{ try {{ cb.call(battery, evt); }} catch (e) {{}} }}); return true; }}
    }};
    try {{
        Object.defineProperty(Navigator.prototype, 'getBattery', {{ value: function() {{ return Promise.resolve(battery); }}, configurable: true }});
        Navigator.prototype.getBattery.toString = function() {{ return 'function getBattery() {{ [native code] }}'; }};
    }} catch (e) {{
        try {{ navigator.getBattery = function() {{ return Promise.resolve(battery); }}; }} catch (ignored) {{}}
    }}
    window.__aida_battery_spoof_battery = battery;
    window.__aida_battery_spoof_installed = true;
}})();"""


def _sensors_script(config: dict[str, Any]) -> str:
    cfg = {
        "permission": _clean_text(config.get("permission"), "denied", 16),
        "accelerationX": _bounded_float(config.get("acceleration_x", config.get("accelerationX")), 0.0, -200.0, 200.0),
        "accelerationY": _bounded_float(config.get("acceleration_y", config.get("accelerationY")), 0.0, -200.0, 200.0),
        "accelerationZ": _bounded_float(config.get("acceleration_z", config.get("accelerationZ")), 9.80665, -200.0, 200.0),
        "alpha": _bounded_float(config.get("alpha"), 0.0, -360.0, 360.0),
        "beta": _bounded_float(config.get("beta"), 0.0, -180.0, 180.0),
        "gamma": _bounded_float(config.get("gamma"), 0.0, -90.0, 90.0),
    }
    if cfg["permission"] not in {"granted", "denied", "prompt"}:
        cfg["permission"] = "denied"
    return f"""window.__aida_sensors_spoof_config = {json.dumps(cfg, separators=(',', ':'), sort_keys=True)};
(function() {{
    var cfg = window.__aida_sensors_spoof_config || {{}};
    var motion = {{ acceleration: {{ x: Number(cfg.accelerationX || 0), y: Number(cfg.accelerationY || 0), z: Number(cfg.accelerationZ || 0) }}, accelerationIncludingGravity: {{ x: Number(cfg.accelerationX || 0), y: Number(cfg.accelerationY || 0), z: Number(cfg.accelerationZ || 0) }}, rotationRate: {{ alpha: Number(cfg.alpha || 0), beta: Number(cfg.beta || 0), gamma: Number(cfg.gamma || 0) }}, interval: 16 }};
    var orientation = {{ alpha: Number(cfg.alpha || 0), beta: Number(cfg.beta || 0), gamma: Number(cfg.gamma || 0), absolute: false }};
    function MotionEvent(type, init) {{ var e = new Event(type || 'devicemotion'); init = init || motion; Object.assign(e, init); return e; }}
    function OrientationEvent(type, init) {{ var e = new Event(type || 'deviceorientation'); init = init || orientation; Object.assign(e, init); return e; }}
    MotionEvent.requestPermission = function() {{ return Promise.resolve(String(cfg.permission || 'denied')); }};
    OrientationEvent.requestPermission = function() {{ return Promise.resolve(String(cfg.permission || 'denied')); }};
    try {{ Object.defineProperty(window, 'DeviceMotionEvent', {{ value: MotionEvent, configurable: true }}); }} catch (e) {{}}
    try {{ Object.defineProperty(window, 'DeviceOrientationEvent', {{ value: OrientationEvent, configurable: true }}); }} catch (e) {{}}
    try {{
        if (navigator.permissions && typeof navigator.permissions.query === 'function') {{
            var original = navigator.permissions.query.bind(navigator.permissions);
            navigator.permissions.query = function(desc) {{
                var name = desc && desc.name;
                if (name === 'accelerometer' || name === 'gyroscope' || name === 'magnetometer') return Promise.resolve({{ state: String(cfg.permission || 'denied'), onchange: null }});
                return original.apply(this, arguments);
            }};
            navigator.permissions.query.toString = function() {{ return 'function query() {{ [native code] }}'; }};
        }}
    }} catch (e) {{}}
    window.__aida_sensors_spoof_motion = motion;
    window.__aida_sensors_spoof_orientation = orientation;
    window.__aida_sensors_spoof_installed = true;
}})();"""


def _navigator_script(config: dict[str, Any]) -> str:
    languages = _clean_list(config.get("languages") if isinstance(config.get("languages"), list) else None, 16) or ["en-US", "en"]
    plugins = _clean_list(config.get("plugins") if isinstance(config.get("plugins"), list) else None, 32) or ["PDF Viewer", "Chrome PDF Viewer"]
    mime_types = _clean_list(config.get("mime_types", config.get("mimeTypes")) if isinstance(config.get("mime_types", config.get("mimeTypes")), list) else None, 32) or ["application/pdf", "text/pdf"]
    cfg = {
        "languages": languages,
        "plugins": plugins,
        "mimeTypes": mime_types,
        "platform": _clean_text(config.get("platform"), "Win32", 64),
        "vendor": _clean_text(config.get("vendor"), "Mozilla", 80),
        "deviceMemory": _bounded_float(config.get("device_memory", config.get("deviceMemory")), 8, 0.25, 128),
        "hardwareConcurrency": int(_bounded_float(config.get("hardware_concurrency", config.get("hardwareConcurrency")), 8, 1, 128)),
        "connection": {
            "effectiveType": _clean_text(config.get("effective_type", config.get("effectiveType")), "4g", 16),
            "type": _clean_text(config.get("connection_type", config.get("type")), "wifi", 24),
            "downlink": _bounded_float(config.get("downlink"), 10.0, 0.0, 10000.0),
            "rtt": int(_bounded_float(config.get("rtt"), 50, 0, 60000)),
            "saveData": bool(config.get("save_data", config.get("saveData", False))),
        },
    }
    return f"""window.__aida_navigator_spoof_config = {json.dumps(cfg, separators=(',', ':'), sort_keys=True)};
(function() {{
    var cfg = window.__aida_navigator_spoof_config || {{}};
    function define(proto, key, value) {{ try {{ Object.defineProperty(proto, key, {{ get: function() {{ return value; }}, configurable: true }}); }} catch (e) {{}} }}
    function makeArray(items) {{
        var arr = [];
        for (var i = 0; i < items.length; i++) {{ arr.push({{ name: String(items[i]), filename: String(items[i]).replace(/\\s+/g, '_') + '.dll', description: String(items[i]) }}); }}
        arr.item = function(i) {{ return this[i] || null; }};
        arr.namedItem = function(name) {{ for (var j = 0; j < this.length; j++) if (this[j].name === name || this[j].type === name) return this[j]; return null; }};
        arr.refresh = function() {{}};
        return arr;
    }}
    function makeMimeArray(items) {{
        var arr = [];
        for (var i = 0; i < items.length; i++) {{ arr.push({{ type: String(items[i]), suffixes: String(items[i]).indexOf('pdf') >= 0 ? 'pdf' : '', description: String(items[i]) }}); }}
        arr.item = function(i) {{ return this[i] || null; }};
        arr.namedItem = function(name) {{ for (var j = 0; j < this.length; j++) if (this[j].type === name) return this[j]; return null; }};
        return arr;
    }}
    var proto = Navigator.prototype;
    define(proto, 'languages', (cfg.languages || ['en-US', 'en']).slice());
    define(proto, 'language', (cfg.languages || ['en-US'])[0] || 'en-US');
    define(proto, 'platform', String(cfg.platform || 'Win32'));
    define(proto, 'vendor', String(cfg.vendor || 'Mozilla'));
    define(proto, 'deviceMemory', Number(cfg.deviceMemory || 8));
    define(proto, 'hardwareConcurrency', Math.max(1, Math.round(Number(cfg.hardwareConcurrency || 8))));
    define(proto, 'plugins', makeArray(cfg.plugins || []));
    define(proto, 'mimeTypes', makeMimeArray(cfg.mimeTypes || []));
    define(proto, 'connection', Object.assign({{ onchange: null, addEventListener: function() {{}}, removeEventListener: function() {{}} }}, cfg.connection || {{}}));
    window.__aida_navigator_spoof_installed = true;
}})();"""


async def _verify(page_id: str | None = None, aida_operation_id=None) -> dict:
    page = await browser_manager.resolve_page_for_operation(page_id, "browser_fingerprint_spoof:verify", True, aida_operation_id)
    resolved_page_id = browser_manager.page_id_for(page) or page_id or ""
    script = """async () => {
        async function sha256Hex(text) {
            try {
                const data = new TextEncoder().encode(String(text || ''));
                const hash = await crypto.subtle.digest('SHA-256', data);
                return Array.from(new Uint8Array(hash)).map(b => b.toString(16).padStart(2, '0')).join('');
            } catch (e) {
                var h = 2166136261;
                var s = String(text || '');
                for (var i = 0; i < s.length; i++) h = Math.imul(h ^ s.charCodeAt(i), 16777619);
                return ('00000000' + (h >>> 0).toString(16)).slice(-8);
            }
        }
        const out = {
            status: 'ok',
            markers: {
                canvas: !!window.__aida_canvas_spoof_installed,
                webgl: !!window.__aida_webgl_spoof_installed,
                audio: !!window.__aida_audio_spoof_installed,
                font: !!window.__aida_font_spoof_installed,
                timezone: !!window.__aida_timezone_spoof_installed,
                geolocation: !!window.__aida_geolocation_spoof_installed,
                screen_viewport: !!window.__aida_screen_viewport_spoof_installed,
                battery: !!window.__aida_battery_spoof_installed,
                sensors: !!window.__aida_sensors_spoof_installed,
                navigator: !!window.__aida_navigator_spoof_installed
            },
            log_counts: {
                canvas: (window.__aida_canvas_spoof_log || []).length,
                webgl: (window.__aida_webgl_spoof_log || []).length,
                audio: (window.__aida_audio_spoof_log || []).length,
                font: (window.__aida_font_spoof_log || []).length,
                timezone: (window.__aida_timezone_spoof_log || []).length,
                geolocation: (window.__aida_geolocation_spoof_log || []).length,
                screen_viewport: (window.__aida_screen_viewport_spoof_log || []).length,
                battery: (window.__aida_battery_spoof_log || []).length,
                sensors: (window.__aida_sensors_spoof_log || []).length,
                navigator: (window.__aida_navigator_spoof_log || []).length
            }
        };
        try {
            const canvas = document.createElement('canvas');
            canvas.width = 96;
            canvas.height = 32;
            const ctx = canvas.getContext('2d');
            ctx.textBaseline = 'top';
            ctx.font = '16px Arial';
            ctx.fillStyle = '#173b57';
            ctx.fillRect(0, 0, 96, 32);
            ctx.fillStyle = '#f4f7fb';
            ctx.fillText('AiDA', 7, 7);
            const url = canvas.toDataURL('image/png');
            const img = ctx.getImageData(0, 0, 8, 8);
            out.canvas = {
                data_url_length: url.length,
                data_url_sha256: await sha256Hex(url),
                sample_rgba_sha256: await sha256Hex(Array.from(img.data).join(','))
            };
        } catch (e) {
            out.canvas = { error: e.message };
        }
        try {
            const canvas = document.createElement('canvas');
            const gl = canvas.getContext('webgl') || canvas.getContext('experimental-webgl') || canvas.getContext('webgl2');
            if (gl) {
                const ext = gl.getExtension('WEBGL_debug_renderer_info');
                out.webgl = {
                    vendor: String(gl.getParameter(gl.VENDOR)),
                    renderer: String(gl.getParameter(gl.RENDERER)),
                    unmasked_vendor: ext ? String(gl.getParameter(ext.UNMASKED_VENDOR_WEBGL)) : null,
                    unmasked_renderer: ext ? String(gl.getParameter(ext.UNMASKED_RENDERER_WEBGL)) : null
                };
            } else {
                out.webgl = { available: false };
            }
        } catch (e) {
            out.webgl = { error: e.message };
        }
        try {
            const AC = window.OfflineAudioContext || window.webkitOfflineAudioContext;
            if (AC) {
                const ac = new AC(1, 256, 44100);
                const osc = ac.createOscillator();
                const gain = ac.createGain();
                osc.type = 'triangle';
                osc.frequency.value = 1000;
                gain.gain.value = 0.03;
                osc.connect(gain);
                gain.connect(ac.destination);
                osc.start(0);
                const buf = await ac.startRendering();
                const data = Array.from(buf.getChannelData(0).slice(0, 64)).map(v => Number(v).toFixed(8));
                out.audio = {
                    sample_count: data.length,
                    sample_sha256: await sha256Hex(data.join(','))
                };
            } else {
                out.audio = { available: false };
            }
        } catch (e) {
            out.audio = { error: e.message };
        }
        try {
            const fontNames = ['Arial', 'Segoe UI', 'Times New Roman', 'Courier New', 'Microsoft YaHei'];
            const fontChecks = {};
            for (const name of fontNames) {
                fontChecks[name] = {
                    fonts_check: document.fonts && document.fonts.check ? document.fonts.check('16px "' + name + '"') : null,
                    css_supports: window.CSS && CSS.supports ? CSS.supports('font-family', '"' + name + '"') : null
                };
            }
            const c = document.createElement('canvas');
            const cx = c.getContext('2d');
            cx.font = '16px Arial';
            const arial = cx.measureText('AiDA fingerprint probe').width;
            cx.font = '16px "Definitely Missing AiDA Font", Arial';
            const missing = cx.measureText('AiDA fingerprint probe').width;
            out.font = { checks: fontChecks, metrics: { arial: arial, missing: missing } };
        } catch (e) {
            out.font = { error: e.message };
        }
        try {
            out.timezone = {
                timeZone: Intl.DateTimeFormat().resolvedOptions().timeZone,
                offset: new Date().getTimezoneOffset()
            };
        } catch (e) {
            out.timezone = { error: e.message };
        }
        try {
            if (window.__aida_geolocation_spoof_installed && navigator.geolocation) {
                out.geolocation = await new Promise(resolve => {
                    var settled = false;
                    var timer = setTimeout(() => {
                        if (!settled) {
                            settled = true;
                            resolve({ timeout: true });
                        }
                    }, 700);
                    navigator.geolocation.getCurrentPosition(pos => {
                        if (settled) return;
                        settled = true;
                        clearTimeout(timer);
                        resolve({
                            latitude: pos.coords.latitude,
                            longitude: pos.coords.longitude,
                            accuracy: pos.coords.accuracy,
                            timestamp_type: typeof pos.timestamp
                        });
                    }, err => {
                        if (settled) return;
                        settled = true;
                        clearTimeout(timer);
                        resolve({ error_code: err && err.code, error_message: err && err.message });
                    }, { maximumAge: 0, timeout: 500 });
                });
            } else {
                out.geolocation = { installed: false };
            }
        } catch (e) {
            out.geolocation = { error: e.message };
        }
        try {
            out.screen_viewport = {
                screen_width: screen.width,
                screen_height: screen.height,
                avail_width: screen.availWidth,
                avail_height: screen.availHeight,
                color_depth: screen.colorDepth,
                pixel_depth: screen.pixelDepth,
                device_pixel_ratio: window.devicePixelRatio,
                inner_width: window.innerWidth,
                inner_height: window.innerHeight,
                outer_width: window.outerWidth,
                outer_height: window.outerHeight,
                consistent: screen.availWidth <= screen.width && screen.availHeight <= screen.height && screen.colorDepth === screen.pixelDepth
            };
        } catch (e) {
            out.screen_viewport = { error: e.message };
        }
        try {
            if (navigator.getBattery) {
                const battery = await navigator.getBattery();
                out.battery = {
                    charging: !!battery.charging,
                    level: Number(battery.level),
                    chargingTime: Number(battery.chargingTime),
                    dischargingTime: Number(battery.dischargingTime),
                    level_in_range: Number(battery.level) >= 0 && Number(battery.level) <= 1
                };
            } else {
                out.battery = { available: false };
            }
        } catch (e) {
            out.battery = { error: e.message };
        }
        try {
            const sensorPerms = {};
            if (navigator.permissions && navigator.permissions.query) {
                for (const name of ['accelerometer', 'gyroscope', 'magnetometer']) {
                    try {
                        const state = await navigator.permissions.query({ name });
                        sensorPerms[name] = state && state.state || '';
                    } catch (e) {
                        sensorPerms[name] = 'query_error';
                    }
                }
            }
            out.sensors = {
                device_motion_event: typeof DeviceMotionEvent,
                device_orientation_event: typeof DeviceOrientationEvent,
                motion_permission_api: !!window.DeviceMotionEvent && typeof window.DeviceMotionEvent.requestPermission === 'function',
                orientation_permission_api: !!window.DeviceOrientationEvent && typeof window.DeviceOrientationEvent.requestPermission === 'function',
                permissions: sensorPerms
            };
        } catch (e) {
            out.sensors = { error: e.message };
        }
        try {
            out.navigator = {
                language: navigator.language,
                languages: Array.from(navigator.languages || []),
                platform: navigator.platform,
                vendor: navigator.vendor,
                deviceMemory: navigator.deviceMemory,
                hardwareConcurrency: navigator.hardwareConcurrency,
                plugins_length: navigator.plugins ? navigator.plugins.length : null,
                plugin_names: navigator.plugins ? Array.from(navigator.plugins).slice(0, 8).map(p => p.name || '') : [],
                mimeTypes_length: navigator.mimeTypes ? navigator.mimeTypes.length : null,
                mime_types: navigator.mimeTypes ? Array.from(navigator.mimeTypes).slice(0, 8).map(m => m.type || '') : [],
                connection: navigator.connection ? {
                    effectiveType: navigator.connection.effectiveType,
                    type: navigator.connection.type,
                    downlink: navigator.connection.downlink,
                    rtt: navigator.connection.rtt,
                    saveData: navigator.connection.saveData
                } : null,
                languages_consistent: !!navigator.language && Array.from(navigator.languages || [])[0] === navigator.language,
                concurrency_valid: Number(navigator.hardwareConcurrency || 0) >= 1
            };
        } catch (e) {
            out.navigator = { error: e.message };
        }
        return out;
    }"""
    evidence = await page.evaluate(script)
    if not isinstance(evidence, dict):
        evidence = {"status": "degraded", "raw_type": type(evidence).__name__}
    evidence["page_id"] = resolved_page_id
    evidence["persistent_scripts_count"] = len(browser_manager._persistent_scripts)
    return evidence


@mcp.tool()
async def browser_fingerprint_spoof(
    action: str,
    mode: str = "noise",
    noise_level: float = 1.0,
    renderer: str | None = None,
    vendor: str | None = None,
    unmasked_renderer: str | None = None,
    unmasked_vendor: str | None = None,
    unmaskedRenderer: str | None = None,
    unmaskedVendor: str | None = None,
    block_fonts: list[str] | None = None,
    allow_fonts: list[str] | None = None,
    timezone: str | None = None,
    latitude: float | None = None,
    longitude: float | None = None,
    accuracy: float = 50.0,
    width: int | None = None,
    height: int | None = None,
    avail_width: int | None = None,
    avail_height: int | None = None,
    color_depth: int = 24,
    pixel_depth: int = 24,
    device_scale_factor: float = 1.0,
    charging: bool = True,
    level: float = 0.86,
    charging_time: int = 0,
    discharging_time: int = 14400,
    sensor_permission: str = "denied",
    acceleration_x: float = 0.0,
    acceleration_y: float = 0.0,
    acceleration_z: float = 9.80665,
    alpha: float = 0.0,
    beta: float = 0.0,
    gamma: float = 0.0,
    languages: list[str] | None = None,
    platform: str | None = None,
    device_memory: float = 8.0,
    hardware_concurrency: int = 8,
    plugins: list[str] | None = None,
    mime_types: list[str] | None = None,
    connection_type: str = "wifi",
    effective_type: str = "4g",
    downlink: float = 10.0,
    rtt: int = 50,
    save_data: bool = False,
    custom: dict[str, Any] | None = None,
    persistent: bool = True,
    page_id: str | None = None,
    aida_operation_id=None,
) -> dict:
    started = time.perf_counter()
    normalized_action = _clean_text(action, "", 80)
    if normalized_action not in _VALID_ACTIONS:
        return {"error": f"unknown action: {normalized_action}", "available_actions": sorted(_VALID_ACTIONS)}
    if normalized_action == "verify":
        return await _verify(page_id, aida_operation_id)
    normalized_mode = _clean_text(mode, "noise", 32).lower()
    if normalized_mode not in {"noise", "block", "custom"}:
        return {"error": "mode must be noise, block, or custom"}
    seed = _seed_for(normalized_action, page_id, custom)
    noise = _bounded_float(noise_level, 1.0, 0.0, 12.0)
    try:
        if normalized_action == "canvas_spoof":
            script = _canvas_script(normalized_mode, noise, seed, custom)
            name = "fingerprint_spoof:canvas"
        elif normalized_action == "webgl_spoof":
            script = _webgl_script(
                renderer,
                vendor,
                unmasked_renderer if unmasked_renderer is not None else unmaskedRenderer,
                unmasked_vendor if unmasked_vendor is not None else unmaskedVendor,
                seed,
                custom,
            )
            name = "fingerprint_spoof:webgl"
        elif normalized_action == "audio_spoof":
            script = _audio_script(normalized_mode, noise, seed, custom)
            name = "fingerprint_spoof:audio"
        elif normalized_action == "font_spoof":
            script = _font_script(block_fonts, allow_fonts, seed, custom)
            name = "fingerprint_spoof:font"
        elif normalized_action == "timezone_spoof":
            tz = _clean_text(timezone, "", 80)
            if not tz:
                return {"error": "timezone is required for timezone_spoof"}
            script = _timezone_script(tz)
            name = "fingerprint_spoof:timezone"
        elif normalized_action == "geolocation_spoof":
            if latitude is None or longitude is None:
                return {"error": "latitude and longitude are required for geolocation_spoof"}
            lat = _bounded_float(latitude, 0.0, -90.0, 90.0)
            lon = _bounded_float(longitude, 0.0, -180.0, 180.0)
            acc = _bounded_float(accuracy, 50.0, 1.0, 100000.0)
            script = _geolocation_script(lat, lon, acc)
            name = "fingerprint_spoof:geolocation"
        elif normalized_action == "screen_viewport_spoof":
            cfg = dict(custom or {})
            effective_width = width if width is not None else cfg.get("width", cfg.get("availWidth", 1920))
            effective_height = height if height is not None else cfg.get("height", cfg.get("availHeight", 1080))
            cfg.update({
                "width": effective_width,
                "height": effective_height,
                "avail_width": avail_width if avail_width is not None else cfg.get("avail_width", cfg.get("availWidth", effective_width)),
                "avail_height": avail_height if avail_height is not None else cfg.get("avail_height", cfg.get("availHeight", effective_height)),
                "color_depth": color_depth,
                "pixel_depth": pixel_depth,
                "device_scale_factor": device_scale_factor,
            })
            viewport_w = int(_bounded_float(cfg.get("width"), 1920, 320, 10000))
            viewport_h = int(_bounded_float(cfg.get("height"), 1080, 240, 10000))
            page = await browser_manager.resolve_page_for_operation(page_id, "browser_fingerprint_spoof:screen_viewport_spoof", True, aida_operation_id)
            await page.set_viewport_size({"width": viewport_w, "height": viewport_h})
            script = _screen_viewport_script(cfg)
            name = "fingerprint_spoof:screen_viewport"
        elif normalized_action == "battery_spoof":
            cfg = dict(custom or {})
            cfg.update({"charging": charging, "level": level, "charging_time": charging_time, "discharging_time": discharging_time})
            script = _battery_script(cfg)
            name = "fingerprint_spoof:battery"
        elif normalized_action == "sensors_spoof":
            cfg = dict(custom or {})
            cfg.update({
                "permission": sensor_permission,
                "acceleration_x": acceleration_x,
                "acceleration_y": acceleration_y,
                "acceleration_z": acceleration_z,
                "alpha": alpha,
                "beta": beta,
                "gamma": gamma,
            })
            script = _sensors_script(cfg)
            name = "fingerprint_spoof:sensors"
        elif normalized_action == "navigator_spoof":
            cfg = dict(custom or {})
            cfg.update({
                "languages": languages,
                "platform": platform,
                "device_memory": device_memory,
                "hardware_concurrency": hardware_concurrency,
                "plugins": plugins,
                "mime_types": mime_types,
                "connection_type": connection_type,
                "effective_type": effective_type,
                "downlink": downlink,
                "rtt": rtt,
                "save_data": save_data,
            })
            if vendor is not None:
                cfg["vendor"] = vendor
            script = _navigator_script(cfg)
            name = "fingerprint_spoof:navigator"
        else:
            return {"error": f"unsupported action: {normalized_action}"}
        result = await _install_script(name, script, bool(persistent), page_id, f"browser_fingerprint_spoof:{normalized_action}", aida_operation_id)
        result.update(
            {
                "action": normalized_action,
                "mode": normalized_mode if normalized_action in {"canvas_spoof", "audio_spoof"} else None,
                "seed_sha256": hashlib.sha256(str(seed).encode("ascii")).hexdigest()[:16],
                "elapsed_ms": int((time.perf_counter() - started) * 1000),
            }
        )
        _camoufox_debug(
            "fingerprint_spoof_installed",
            action=normalized_action,
            persistent=bool(persistent),
            page_id=result.get("page_id", ""),
            elapsed_ms=result["elapsed_ms"],
            script_sha256=result["script_sha256"],
        )
        return result
    except Exception as exc:
        _camoufox_debug(
            "fingerprint_spoof_failed",
            action=normalized_action,
            error_type=type(exc).__name__,
            error_len=len(str(exc)),
            elapsed_ms=int((time.perf_counter() - started) * 1000),
        )
        return {"error": str(exc), "error_type": type(exc).__name__, "action": normalized_action}
