'use strict';
// CommonJS, deliberately independent of Node APIs: runs in the game's PuerTS VM.
const ID = /^[A-Za-z0-9_-]{1,64}$/;
const ENTRY = /^\.\/(?:mods|generated-reloads\/[0-9]+\/generated-mods)\/[A-Za-z0-9_-]{1,64}\/main\.js$/;
function validate(config) {
    if (!config || config.version !== 1 || !Array.isArray(config.mods) || config.mods.length > 128) {
        throw new Error('Unsupported MOD configuration');
    }
    const ids = new Set();
    for (const mod of config.mods) {
        if (!mod || typeof mod.id !== 'string' || !ID.test(mod.id) || ids.has(mod.id.toLowerCase()) ||
            typeof mod.enabled !== 'boolean' || typeof mod.entry !== 'string' ||
            mod.entry.length > 512 || !ENTRY.test(mod.entry)) {
            throw new Error('Invalid or duplicate MOD: ' + (mod && mod.id));
        }
        if (mod.entry.split('/').slice(-2, -1)[0].toLowerCase() !== mod.id.toLowerCase()) {
            throw new Error('MOD entry does not match ID: ' + mod.id);
        }
        ids.add(mod.id.toLowerCase());
    }
}
exports.load = function load(config, importMod, log) {
    validate(config);
    const result = [];
    for (const mod of config.mods) {
        if (!mod.enabled) {
            result.push({ id: mod.id, status: 'disabled' });
            log('disabled ' + mod.id);
            continue;
        }
        try {
            const instance = importMod(mod.entry);
            result.push({ id: mod.id, status: 'loaded', instance });
            log('loaded ' + mod.id);
        } catch (error) {
            result.push({ id: mod.id, status: 'failed' });
            log('failed ' + mod.id + ': ' + String(error && error.stack || error));
        }
    }
    return result;
};

// A failed import may already have registered callbacks. Do not retry it in this
// session: there is no exported instance with which to clean up those effects.
function stopWithTimeout(instance, timeoutMs) {
    const stopping = Promise.resolve().then(() => instance.stop());
    // A MOD may finish cleanup after the timeout. Its result no longer matters,
    // but an unhandled rejection would still surface in the game console.
    stopping.catch(() => {});
    let timer;
    const expired = new Promise((_, reject) => {
        timer = setTimeout(() => { const error = new Error('stop() timed out after ' + timeoutMs + 'ms; MOD kept blocked'); error.timeout = true; reject(error); }, timeoutMs);
    });
    return Promise.race([stopping, expired]).then(
        value => { clearTimeout(timer); return value; },
        error => { clearTimeout(timer); throw error; });
}

exports.reload = async function reload(previous, config, importMod, log, stopTimeoutMs = 5000) {
    validate(config);
    const blocked = new Map();
    for (const mod of previous) {
        if (mod.status === 'disabled' || mod.status === 'stopped') continue;
        if (mod.status === 'failed') { blocked.set(mod.id, mod); continue; }
        try {
            if (!mod.instance || typeof mod.instance.stop !== 'function') {
                throw new Error('export stop() to enable hot reload');
            }
            await stopWithTimeout(mod.instance, stopTimeoutMs);
            delete mod.reloadError;
            mod.status = 'stopped';
            log('stopped ' + mod.id);
        } catch (error) {
            mod.reloadError = true;
            if (error && error.timeout) mod.status = 'failed';
            blocked.set(mod.id, mod);
            log('reload blocked ' + mod.id + ': ' + String(error && error.stack || error));
        }
    }
    const next = [];
    for (const mod of config.mods) {
        if (blocked.has(mod.id)) {
            next.push(blocked.get(mod.id));
            blocked.delete(mod.id);
        } else {
            next.push(...exports.load({ version: 1, mods: [mod] }, importMod, log));
        }
    }
    // Keep failed cleanup instances reachable even if removed from configuration.
    return next.concat(Array.from(blocked.values()));
};

exports.watch = function watch(state, importMod, log, schedule = setInterval, notify = () => {}) {
    let revision = 1, busy = false;
    return schedule(async () => {
        if (busy) return;
        busy = true;
        try {
            let config;
            const prefix = './generated-reloads/' + revision + '/';
            // Publication is atomic. A missing next revision is the normal idle state.
            try { config = importMod(prefix + 'config'); } catch (_) { return; }
            if (config.error) throw new Error(config.error);
            state.mods = await exports.reload(state.mods, config, importMod, log);
            log('reload complete revision=' + revision);
            const failed = state.mods.filter(mod => mod.status === 'failed' || mod.reloadError).length;
            const loaded = state.mods.filter(mod => mod.status === 'loaded' && !mod.reloadError).length;
            notify(failed ? 'MOD 重载完成：成功 ' + loaded + '，失败或跳过 ' + failed : 'MOD 重载成功（' + loaded + ' 个）', failed > 0);
            revision++;
        } catch (error) {
            log('reload failed: ' + String(error && error.stack || error));
            notify('MOD 重载失败，请查看 loader.log', true);
            revision++;
        } finally { busy = false; }
    }, 1000);
};

// Use a real UMG overlay: debug PrintString may be compiled out in Shipping.
exports.createNotifier = function createNotifier(importMod, log, delay = setTimeout, cancel = clearTimeout) {
    let widget, viewport, timer;
    return (message, failed) => {
        try {
            const UE = importMod('ue');
            const gi = importMod('puerts').argv.getByName('GameInstance');
            if (timer !== undefined) { cancel(timer); timer = undefined; }
            if (widget && viewport) { viewport.RemoveWidget(widget); widget = undefined; }
            viewport = UE.SubsystemBlueprintLibrary.GetEngineSubsystem(UE.GameViewportSubsystem.StaticClass());
            if (!viewport || !gi) throw new Error('viewport unavailable');
            widget = new UE.TextBlock(gi);
            widget.SetText(UE.KismetTextLibrary.Conv_StringToText('[PBZModLoader] ' + message));
            widget.SetVisibility(UE.ESlateVisibility.HitTestInvisible);
            const color = new UE.SlateColor();
            const tint = new UE.LinearColor();
            tint.R = failed ? 1 : 0.3; tint.G = failed ? 0.65 : 1; tint.B = 0.3; tint.A = 1;
            color.SpecifiedColor = tint;
            widget.SetColorAndOpacity(color);
            const shadow = new UE.LinearColor(); shadow.A = 1;
            widget.SetShadowColorAndOpacity(shadow);
            const shadowOffset = new UE.Vector2D(); shadowOffset.X = 1; shadowOffset.Y = 1;
            widget.SetShadowOffset(shadowOffset);
            const slot = new UE.GameViewportWidgetSlot();
            // Reflected structs do not all implement positional constructors.
            const offsets = new UE.Margin();
            offsets.Left = 24; offsets.Top = 24; offsets.Right = 800; offsets.Bottom = 50;
            slot.Offsets = offsets;
            slot.ZOrder = 1000000;
            slot.bAutoRemoveOnWorldRemoved = true;
            if (!viewport.AddWidget(widget, slot)) throw new Error('AddWidget failed');
            timer = delay(() => {
                timer = undefined;
                try { if (widget) viewport.RemoveWidget(widget); }
                catch (error) { log('notification cleanup failed: ' + String(error)); }
                widget = undefined;
            }, 4000);
        } catch (error) {
            log('notification unavailable: ' + String(error));
        }
    };
};
