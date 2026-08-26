(function() {
    var css = [
        "@font-face{font-family:'Microsoft YaHei';src:local('Microsoft YaHei'),local('PingFang SC'),local('Hiragino Sans GB'),local('Noto Sans CJK SC'),local('WenQuanYi Micro Hei'),local('sans-serif')}",
        "@font-face{font-family:'\\5FAE\\8F6F\\96C5\\9ED1';src:local('\\5FAE\\8F6F\\96C5\\9ED1'),local('PingFang SC'),local('Hiragino Sans GB'),local('Noto Sans CJK SC'),local('WenQuanYi Micro Hei')}",
        "@font-face{font-family:'SimSun';src:local('SimSun'),local('Songti SC'),local('STSong'),local('Noto Serif CJK SC'),local('AR PL UMing CN')}",
        "@font-face{font-family:'\\5B8B\\4F53';src:local('\\5B8B\\4F53'),local('Songti SC'),local('STSong'),local('Noto Serif CJK SC'),local('AR PL UMing CN')}",
        "@font-face{font-family:'SimHei';src:local('SimHei'),local('PingFang SC'),local('Heiti SC'),local('Noto Sans CJK SC'),local('WenQuanYi Zen Hei')}",
        "@font-face{font-family:'\\9ED1\\4F53';src:local('\\9ED1\\4F53'),local('PingFang SC'),local('Heiti SC'),local('Noto Sans CJK SC'),local('WenQuanYi Zen Hei')}",
        "@font-face{font-family:'KaiTi';src:local('KaiTi'),local('Kaiti SC'),local('STKaiti'),local('Noto Sans CJK SC'),local('AR PL UKai CN')}",
        "@font-face{font-family:'\\6977\\4F53';src:local('\\6977\\4F53'),local('Kaiti SC'),local('STKaiti'),local('Noto Sans CJK SC'),local('AR PL UKai CN')}",
        "@font-face{font-family:'PingFang SC';src:local('PingFang SC'),local('Microsoft YaHei'),local('Noto Sans CJK SC'),local('WenQuanYi Micro Hei')}",
        "@font-face{font-family:'Hiragino Sans GB';src:local('Hiragino Sans GB'),local('Microsoft YaHei'),local('Noto Sans CJK SC'),local('WenQuanYi Micro Hei')}",
        "@font-face{font-family:'Heiti SC';src:local('Heiti SC'),local('Microsoft YaHei'),local('SimHei'),local('Noto Sans CJK SC')}",
        "@font-face{font-family:'Noto Sans CJK SC';src:local('Noto Sans CJK SC'),local('PingFang SC'),local('Microsoft YaHei'),local('WenQuanYi Micro Hei')}",
        "@font-face{font-family:'WenQuanYi Micro Hei';src:local('WenQuanYi Micro Hei'),local('PingFang SC'),local('Microsoft YaHei'),local('Noto Sans CJK SC')}"
    ].join("\n");
    try {
        if (!document.getElementById('__aida_font_fallback_style')) {
            var style = document.createElement("style");
            style.id = '__aida_font_fallback_style';
            style.textContent = css;
            (document.head || document.documentElement).appendChild(style);
        }
    } catch (e) {}
    if (window.__aida_font_spoof_installed) return;
    var cfg = window.__aida_font_spoof_config || {};
    var log = window.__aida_font_spoof_log = window.__aida_font_spoof_log || [];
    var originals = window.__aida_font_spoof_originals = {};
    var maxLog = 320;
    function current() {
        return window.__aida_font_spoof_config || cfg || {};
    }
    function normalize(name) {
        return String(name || '').replace(/^['"]|['"]$/g, '').trim().toLowerCase();
    }
    function configured(list) {
        var out = {};
        if (!Array.isArray(list)) return out;
        for (var i = 0; i < list.length; i++) {
            var n = normalize(list[i]);
            if (n) out[n] = true;
        }
        return out;
    }
    function families(text) {
        var out = [];
        var re = /"([^"]+)"|'([^']+)'|([A-Za-z0-9 _.-]+)/g;
        var m;
        while ((m = re.exec(String(text || '')))) {
            var value = normalize(m[1] || m[2] || m[3]);
            if (value && value !== 'serif' && value !== 'sans-serif' && value !== 'monospace' && value !== 'cursive' && value !== 'fantasy' && value !== 'system-ui') out.push(value);
        }
        return out;
    }
    function policyFor(text) {
        var c = current();
        var allow = configured(c.allowFonts);
        var block = configured(c.blockFonts);
        var fams = families(text);
        if (!fams.length) return null;
        for (var i = 0; i < fams.length; i++) {
            if (block[fams[i]]) return false;
        }
        var allowKeys = Object.keys(allow);
        if (allowKeys.length) {
            for (var j = 0; j < fams.length; j++) {
                if (allow[fams[j]]) return true;
            }
            return false;
        }
        return null;
    }
    function push(entry) {
        if (log.length >= maxLog) log.shift();
        entry.timestamp = Date.now();
        try { entry.stack = (new Error().stack || '').split('\n').slice(2, 7).join('\n'); } catch (e) {}
        log.push(entry);
    }
    function fontFromCssSupports(args) {
        if (args.length === 1) {
            var text = String(args[0] || '');
            var idx = text.toLowerCase().indexOf('font-family');
            return idx >= 0 ? text.slice(idx) : '';
        }
        if (String(args[0] || '').toLowerCase() === 'font-family') return String(args[1] || '');
        return '';
    }
    function fontSize(styleText) {
        var m = String(styleText || '').match(/(\d+(?:\.\d+)?)px/);
        return m ? Number(m[1]) : 16;
    }
    function normalizedWidth(text, font) {
        var size = fontSize(font);
        return Math.max(1, Math.round(String(text || '').length * size * 0.56));
    }
    function normalizedHeight(font) {
        return Math.max(1, Math.round(fontSize(font) * 1.18));
    }
    try {
        if (document.fonts && document.fonts.check) {
            var fontSetProto = Object.getPrototypeOf(document.fonts);
            originals.fontsCheck = fontSetProto.check;
            fontSetProto.check = function(font, text) {
                var policy = policyFor(font);
                if (policy !== null) {
                    push({ method: 'document.fonts.check', font: String(font).slice(0, 160), result: policy });
                    return policy;
                }
                return originals.fontsCheck.apply(this, arguments);
            };
            fontSetProto.check.toString = function() { return 'function check() { [native code] }'; };
        }
    } catch (e) {}
    try {
        if (window.CSS && CSS.supports) {
            originals.cssSupports = CSS.supports.bind(CSS);
            CSS.supports = function() {
                var font = fontFromCssSupports(arguments);
                var policy = font ? policyFor(font) : null;
                if (policy !== null) {
                    push({ method: 'CSS.supports', font: String(font).slice(0, 160), result: policy });
                    return policy;
                }
                return originals.cssSupports.apply(this, arguments);
            };
            CSS.supports.toString = function() { return 'function supports() { [native code] }'; };
        }
    } catch (e) {}
    try {
        if (window.CanvasRenderingContext2D && CanvasRenderingContext2D.prototype.measureText) {
            originals.measureText = CanvasRenderingContext2D.prototype.measureText;
            CanvasRenderingContext2D.prototype.measureText = function(text) {
                var result = originals.measureText.apply(this, arguments);
                var policy = policyFor(this.font || '');
                if (policy === false) {
                    var width = normalizedWidth(text, this.font);
                    push({ method: 'CanvasRenderingContext2D.measureText', font: String(this.font || '').slice(0, 160), text_len: String(text || '').length, width: width });
                    try {
                        return new Proxy(result, {
                            get: function(target, prop) {
                                if (prop === 'width') return width;
                                return target[prop];
                            }
                        });
                    } catch (e) {
                        return result;
                    }
                }
                return result;
            };
            CanvasRenderingContext2D.prototype.measureText.toString = function() { return 'function measureText() { [native code] }'; };
        }
    } catch (e) {}
    try {
        var widthDesc = Object.getOwnPropertyDescriptor(HTMLElement.prototype, 'offsetWidth');
        var heightDesc = Object.getOwnPropertyDescriptor(HTMLElement.prototype, 'offsetHeight');
        if (widthDesc && widthDesc.get && heightDesc && heightDesc.get) {
            originals.offsetWidth = widthDesc.get;
            originals.offsetHeight = heightDesc.get;
            Object.defineProperty(HTMLElement.prototype, 'offsetWidth', {
                get: function() {
                    var font = '';
                    try { font = (this.style && this.style.fontFamily) || getComputedStyle(this).fontFamily || ''; } catch (e) {}
                    if (policyFor(font) === false) {
                        var value = normalizedWidth(this.textContent || '', font);
                        push({ method: 'HTMLElement.offsetWidth', font: String(font).slice(0, 160), text_len: String(this.textContent || '').length, width: value });
                        return value;
                    }
                    return originals.offsetWidth.call(this);
                },
                configurable: true
            });
            Object.defineProperty(HTMLElement.prototype, 'offsetHeight', {
                get: function() {
                    var font = '';
                    try { font = (this.style && this.style.fontFamily) || getComputedStyle(this).fontFamily || ''; } catch (e) {}
                    if (policyFor(font) === false) {
                        var value = normalizedHeight(font);
                        push({ method: 'HTMLElement.offsetHeight', font: String(font).slice(0, 160), height: value });
                        return value;
                    }
                    return originals.offsetHeight.call(this);
                },
                configurable: true
            });
        }
    } catch (e) {}
    window.__aida_font_spoof_uninstall = function() {
        try {
            if (document.fonts && originals.fontsCheck) Object.getPrototypeOf(document.fonts).check = originals.fontsCheck;
        } catch (e) {}
        try { if (originals.cssSupports) CSS.supports = originals.cssSupports; } catch (e) {}
        try { if (originals.measureText) CanvasRenderingContext2D.prototype.measureText = originals.measureText; } catch (e) {}
        try {
            if (originals.offsetWidth) Object.defineProperty(HTMLElement.prototype, 'offsetWidth', { get: originals.offsetWidth, configurable: true });
            if (originals.offsetHeight) Object.defineProperty(HTMLElement.prototype, 'offsetHeight', { get: originals.offsetHeight, configurable: true });
        } catch (e) {}
        window.__aida_font_spoof_installed = false;
        window.__aida_font_spoof_uninstalled = true;
        return { restored: ['document.fonts.check', 'CSS.supports', 'CanvasRenderingContext2D.measureText', 'HTMLElement metrics'] };
    };
    window.__aida_font_spoof_installed = true;
    window.__aida_font_spoof_uninstalled = false;
})();
