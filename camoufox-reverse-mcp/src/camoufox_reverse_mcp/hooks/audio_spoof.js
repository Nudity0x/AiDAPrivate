(function() {
    var cfg = window.__aida_audio_spoof_config || {};
    if (window.__aida_audio_spoof_installed) {
        return;
    }
    var log = window.__aida_audio_spoof_log = window.__aida_audio_spoof_log || [];
    var originals = window.__aida_audio_spoof_originals = {};
    var spoofedArrays = new WeakSet();
    var maxLog = 240;
    function current() {
        return window.__aida_audio_spoof_config || cfg || {};
    }
    function push(entry) {
        if (log.length >= maxLog) log.shift();
        entry.timestamp = Date.now();
        try { entry.stack = (new Error().stack || '').split('\n').slice(2, 7).join('\n'); } catch (e) {}
        log.push(entry);
    }
    function rnd(seed, index) {
        var x = (seed >>> 0) ^ Math.imul((index + 17) >>> 0, 2246822519);
        x ^= x << 13;
        x ^= x >>> 17;
        x ^= x << 5;
        return (x >>> 0) / 4294967295;
    }
    function alter(array, tag) {
        var c = current();
        var mode = String(c.mode || 'noise');
        if (!array || spoofedArrays.has(array)) return array;
        if (mode === 'block') {
            for (var b = 0; b < array.length; b++) array[b] = 0;
            spoofedArrays.add(array);
            push({ method: tag, mode: mode, length: array.length });
            return array;
        }
        var level = Math.max(0, Math.min(12, Number(c.noiseLevel || 1)));
        if (!level) return array;
        var seed = Number(c.seed || 1) >>> 0;
        var amplitude = mode === 'custom' && c.custom && c.custom.amplitude != null ? Number(c.custom.amplitude) : (0.0000008 * level);
        for (var i = 0; i < array.length; i++) {
            var delta = (rnd(seed, i) - 0.5) * amplitude;
            array[i] = array[i] + delta;
        }
        spoofedArrays.add(array);
        push({ method: tag, mode: mode, length: array.length, amplitude: amplitude });
        return array;
    }
    try {
        if (window.AnalyserNode) {
            originals.getFloatFrequencyData = AnalyserNode.prototype.getFloatFrequencyData;
            originals.getByteFrequencyData = AnalyserNode.prototype.getByteFrequencyData;
            AnalyserNode.prototype.getFloatFrequencyData = function(array) {
                var out = originals.getFloatFrequencyData.apply(this, arguments);
                alter(array, 'AnalyserNode.getFloatFrequencyData');
                return out;
            };
            AnalyserNode.prototype.getByteFrequencyData = function(array) {
                var out = originals.getByteFrequencyData.apply(this, arguments);
                if (String(current().mode || 'noise') === 'block') {
                    for (var i = 0; array && i < array.length; i++) array[i] = 0;
                }
                push({ method: 'AnalyserNode.getByteFrequencyData', mode: String(current().mode || 'noise'), length: array ? array.length : 0 });
                return out;
            };
            AnalyserNode.prototype.getFloatFrequencyData.toString = function() { return 'function getFloatFrequencyData() { [native code] }'; };
            AnalyserNode.prototype.getByteFrequencyData.toString = function() { return 'function getByteFrequencyData() { [native code] }'; };
        }
    } catch (e) {}
    try {
        if (window.AudioBuffer) {
            originals.getChannelData = AudioBuffer.prototype.getChannelData;
            originals.copyFromChannel = AudioBuffer.prototype.copyFromChannel;
            AudioBuffer.prototype.getChannelData = function() {
                var data = originals.getChannelData.apply(this, arguments);
                return alter(data, 'AudioBuffer.getChannelData');
            };
            AudioBuffer.prototype.copyFromChannel = function(destination) {
                var out = originals.copyFromChannel.apply(this, arguments);
                alter(destination, 'AudioBuffer.copyFromChannel');
                return out;
            };
            AudioBuffer.prototype.getChannelData.toString = function() { return 'function getChannelData() { [native code] }'; };
            AudioBuffer.prototype.copyFromChannel.toString = function() { return 'function copyFromChannel() { [native code] }'; };
        }
    } catch (e) {}
    try {
        if (window.BaseAudioContext) {
            originals.createOscillator = BaseAudioContext.prototype.createOscillator;
            originals.createAnalyser = BaseAudioContext.prototype.createAnalyser;
            BaseAudioContext.prototype.createOscillator = function() {
                push({ method: 'BaseAudioContext.createOscillator', sampleRate: this.sampleRate || null });
                return originals.createOscillator.apply(this, arguments);
            };
            BaseAudioContext.prototype.createAnalyser = function() {
                push({ method: 'BaseAudioContext.createAnalyser', sampleRate: this.sampleRate || null });
                return originals.createAnalyser.apply(this, arguments);
            };
            BaseAudioContext.prototype.createOscillator.toString = function() { return 'function createOscillator() { [native code] }'; };
            BaseAudioContext.prototype.createAnalyser.toString = function() { return 'function createAnalyser() { [native code] }'; };
        }
    } catch (e) {}
    try {
        if (window.OfflineAudioContext) {
            originals.startRendering = OfflineAudioContext.prototype.startRendering;
            OfflineAudioContext.prototype.startRendering = function() {
                push({ method: 'OfflineAudioContext.startRendering', sampleRate: this.sampleRate || null, length: this.length || null });
                return originals.startRendering.apply(this, arguments).then(function(buffer) {
                    try { alter(buffer.getChannelData(0), 'OfflineAudioContext.renderedBuffer'); } catch (e) {}
                    return buffer;
                });
            };
            OfflineAudioContext.prototype.startRendering.toString = function() { return 'function startRendering() { [native code] }'; };
        }
    } catch (e) {}
    window.__aida_audio_spoof_uninstall = function() {
        try { if (originals.getFloatFrequencyData) AnalyserNode.prototype.getFloatFrequencyData = originals.getFloatFrequencyData; } catch (e) {}
        try { if (originals.getByteFrequencyData) AnalyserNode.prototype.getByteFrequencyData = originals.getByteFrequencyData; } catch (e) {}
        try { if (originals.getChannelData) AudioBuffer.prototype.getChannelData = originals.getChannelData; } catch (e) {}
        try { if (originals.copyFromChannel) AudioBuffer.prototype.copyFromChannel = originals.copyFromChannel; } catch (e) {}
        try { if (originals.createOscillator) BaseAudioContext.prototype.createOscillator = originals.createOscillator; } catch (e) {}
        try { if (originals.createAnalyser) BaseAudioContext.prototype.createAnalyser = originals.createAnalyser; } catch (e) {}
        try { if (originals.startRendering) OfflineAudioContext.prototype.startRendering = originals.startRendering; } catch (e) {}
        window.__aida_audio_spoof_installed = false;
        window.__aida_audio_spoof_uninstalled = true;
        return { restored: ['audio prototypes'] };
    };
    window.__aida_audio_spoof_installed = true;
    window.__aida_audio_spoof_uninstalled = false;
})();
