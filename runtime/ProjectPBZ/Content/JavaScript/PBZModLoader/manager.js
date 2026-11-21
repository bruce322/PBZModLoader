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
        const started = nowMs();
        if (!mod.enabled) {
            result.push({ id: mod.id, status: 'disabled', entry: mod.entry, reason: 'disabled in configuration', loadMs: 0 });
            log('disabled ' + mod.id);
            continue;
        }
        try {
            const instance = importMod(mod.entry);
            result.push({ id: mod.id, status: 'loaded', entry: mod.entry, instance, loadMs: elapsed(started) });
            log('loaded ' + mod.id);
        } catch (error) {
            const reason = describeFailure(error);
            result.push({ id: mod.id, status: 'failed', entry: mod.entry, reason, frame: firstFrame(error), loadMs: elapsed(started) });
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
        if (mod.status === 'failed') { blocked.set(mod.id.toLowerCase(), mod); continue; }
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
            mod.reason = describeFailure(error);
            mod.frame = firstFrame(error);
            if (error && error.timeout) mod.status = 'failed';
            blocked.set(mod.id.toLowerCase(), mod);
            log('reload blocked ' + mod.id + ': ' + String(error && error.stack || error));
        }
    }
    const next = [];
    for (const mod of config.mods) {
        const key = mod.id.toLowerCase();
        if (blocked.has(key)) {
            const kept = blocked.get(key);
            blocked.delete(key);
            if (mod.enabled || kept.instance) {
                // A MOD that is still running cannot be unloaded; keep it visible.
                delete kept.retained;
                next.push(kept);
            } else {
                // A failed evaluation that the configuration disabled must not linger
                // as failed, but the old object stays blocked so a later re-enable
                // cannot retry an import that may already have registered callbacks.
                kept.retained = true;
                next.push(...exports.load({ version: 1, mods: [mod] }, importMod, log));
                next.push(kept);
            }
        } else {
            const started = nowMs();
            const outcomes = exports.load({ version: 1, mods: [mod] }, importMod, log);
            const reloadMs = elapsed(started);
            for (const outcome of outcomes) outcome.reloadMs = reloadMs;
            next.push(...outcomes);
        }
    }
    // Cleanup-blocked instances stay reachable even if removed from configuration;
    // evaluation failures are hidden from accounting but stay blocked in-session.
    for (const mod of blocked.values()) {
        if (!mod.instance) mod.retained = true;
        next.push(mod);
    }
    return next;
};

exports.watch = function watch(state, importMod, log, schedule = (typeof setInterval === 'function' ? setInterval : null), notify = () => {}) {
    if (typeof schedule !== 'function') {
        log('hot reload unavailable: setInterval is missing');
        return undefined;
    }
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
            const declared = state.readyIds ? new Set(state.readyIds) : null;
            const previous = state.mods;
            if (state.readyIds) state.readyIds.clear();
            try {
                state.mods = await exports.reload(previous, config, importMod, log);
            } catch (error) {
                // Reload threw before a new state was published: the old declarations still apply.
                if (declared && state.readyIds) for (const id of declared) state.readyIds.add(id);
                throw error;
            }
            // A MOD whose old instance is kept running because cleanup failed still
            // holds its earlier readiness declaration.
            if (declared && state.readyIds) {
                for (const mod of state.mods) {
                    if (!mod || !mod.instance) continue;
                    const key = String(mod.id).toLowerCase();
                    if (!declared.has(key)) continue;
                    const old = (previous || []).find(prev => prev && String(prev.id).toLowerCase() === key);
                    if (old && old.instance === mod.instance) state.readyIds.add(key);
                }
            }
            log('reload complete revision=' + revision);
            const counted = state.mods.filter(mod => mod && !mod.retained);
            const failed = counted.filter(mod => mod.status === 'failed' || mod.reloadError).length;
            const loaded = counted.filter(mod => mod.status === 'loaded' && !mod.reloadError).length;
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
exports.createNotifier = function createNotifier(importMod, log,
    delay = (typeof setTimeout === 'function' ? setTimeout : null),
    cancel = (typeof clearTimeout === 'function' ? clearTimeout : null)) {
    let widget, viewport, timer;
    return (message, failed) => {
        try {
            if (typeof delay !== 'function') { log('notification unavailable: no timer'); return; }
            const UE = importMod('ue');
            const gi = importMod('puerts').argv.getByName('GameInstance');
            if (timer !== undefined) {
                if (typeof cancel === 'function') cancel(timer);
                timer = undefined;
            }
            if (widget && viewport) { viewport.RemoveWidget(widget); widget = undefined; }
            viewport = UE.SubsystemBlueprintLibrary.GetEngineSubsystem(UE.GameViewportSubsystem.StaticClass());
            if (!viewport || !gi) throw new Error('viewport unavailable');
            // Build the overlay first and publish it only after it really joined the viewport,
            // so a failed AddWidget cannot leave a stale handle behind for the next toast.
            const created = new UE.TextBlock(gi);
            created.SetText(UE.KismetTextLibrary.Conv_StringToText('[PBZModLoader] ' + message));
            created.SetVisibility(UE.ESlateVisibility.HitTestInvisible);
            const color = new UE.SlateColor();
            const tint = new UE.LinearColor();
            tint.R = failed ? 1 : 0.3; tint.G = failed ? 0.65 : 1; tint.B = 0.3; tint.A = 1;
            color.SpecifiedColor = tint;
            created.SetColorAndOpacity(color);
            const shadow = new UE.LinearColor(); shadow.A = 1;
            created.SetShadowColorAndOpacity(shadow);
            const shadowOffset = new UE.Vector2D(); shadowOffset.X = 1; shadowOffset.Y = 1;
            created.SetShadowOffset(shadowOffset);
            const slot = new UE.GameViewportWidgetSlot();
            // Reflected structs do not all implement positional constructors.
            const offsets = new UE.Margin();
            offsets.Left = 24; offsets.Top = 24; offsets.Right = 800; offsets.Bottom = 50;
            slot.Offsets = offsets;
            slot.ZOrder = 1000000;
            slot.bAutoRemoveOnWorldRemoved = true;
            if (!viewport.AddWidget(created, slot)) throw new Error('AddWidget failed');
            widget = created;
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
// --- reporting helpers (part of the 1.0.7 loader runtime) ---
function describeFailure(error) {
    const text = String((error && (error.message || error.stack)) || error || 'unknown error');
    const firstLine = text.split(/\r?\n/)[0].trim();
    return firstLine.length > 200 ? firstLine.slice(0, 197) + '...' : firstLine;
}

function firstFrame(error) {
    const stack = String(error && error.stack || '');
    const match = /at\s+([^\n]+)/.exec(stack);
    return match ? match[1].trim() : '';
}

function emit(level, message) {
    try {
        const target = globalThis.console;
        if (!target) return;
        const write = level === 'error' ? (target.error || target.log) : (target.warn || target.log);
        if (typeof write === 'function') write.call(target, '[PBZMOD] ' + message);
    } catch (_) { /* reporting must never break loading */ }
}

exports.countMods = function countMods(mods) {
    const counts = { loaded: 0, failed: 0, disabled: 0, stopped: 0, blocked: 0 };
    for (const mod of mods || []) {
        if (!mod || mod.retained) continue;
        counts[mod.status] = (counts[mod.status] || 0) + 1;
        if (mod.reloadError === true) counts.blocked += 1;
    }
    return counts;
};

exports.markReadiness = function markReadiness(state) {
    const readiness = { confirmed: 0, unconfirmed: 0, unverified: 0 };
    const declared = state.readyIds || new Set();
    for (const mod of state.mods || []) {
        if (!mod) continue;
        if (mod.status !== 'loaded' || mod.retained) { mod.readyState = undefined; continue; }
        const instance = mod.instance;
        let signalled = false;
        let ready = false;
        if (instance && typeof instance.ready !== 'undefined') {
            signalled = true;
            ready = instance.ready === true;
        }
        if (declared.has(String(mod.id).toLowerCase())) {
            signalled = true;
            ready = true;
        }
        mod.readyState = signalled ? (ready ? 'confirmed' : 'unconfirmed') : 'unverified';
        readiness[mod.readyState] += 1;
    }
    state.readiness = readiness;
    return readiness;
};

exports.reportStatus = function reportStatus(state, phase) {
    const counts = exports.countMods(state.mods);
    const readiness = state.readiness || { confirmed: 0, unconfirmed: 0, unverified: 0 };
    const degraded = counts.failed > 0 || counts.blocked > 0 || readiness.unconfirmed > 0 ? 1 : 0;
    const name = phase || 'startup';
    state.lastPhase = name;
    const visible = (state.mods || []).filter(mod => mod && !mod.retained);
    const failedIds = visible.filter(mod => mod.status === 'failed' && mod.reloadError !== true).map(mod => mod.id).join(',');
    const blockedIds = visible.filter(mod => mod.reloadError === true).map(mod => mod.id).join(',');
    const unconfirmedIds = visible.filter(mod => mod.readyState === 'unconfirmed').map(mod => mod.id).join(',');
    const signature = counts.loaded + '|' + counts.failed + '|' + counts.disabled + '|' + counts.blocked + '|' +
        readiness.unconfirmed + '|' + readiness.unverified + '|' + degraded + '|' + failedIds + '|' +
        blockedIds + '|' + unconfirmedIds;
    if (name !== 'reload' && state.statusSignature === signature) {
        return { skipped: true, counts, readiness, degraded };
    }
    state.statusSignature = signature;
    const timed = visible.filter(mod => mod.status === 'loaded' && mod.reloadError !== true &&
        typeof mod.loadMs === 'number' && mod.loadMs > 0);
    let timing = '';
    if (timed.length) {
        timed.sort((a, b) => b.loadMs - a.loadMs);
        timing = ' slowest=' + timed[0].id + ':' + timed[0].loadMs + 'ms';
    }
    emit('warn', 'status phase=' + name +
        ' loaded=' + counts.loaded + ' failed=' + counts.failed +
        ' disabled=' + counts.disabled + ' blocked=' + counts.blocked +
        ' unconfirmed=' + readiness.unconfirmed +
        ' unverified=' + readiness.unverified + ' degraded=' + degraded + timing);
    const failedChanged = failedIds !== (state.lastFailedIds || '');
    const blockedChanged = blockedIds !== (state.lastBlockedIds || '');
    const unconfirmedChanged = unconfirmedIds !== (state.lastUnconfirmedIds || '');
    state.lastFailedIds = failedIds;
    state.lastBlockedIds = blockedIds;
    state.lastUnconfirmedIds = unconfirmedIds;
    for (const mod of visible) {
        if (mod.reloadError === true) {
            if (blockedChanged) emit('error', 'blocked ' + mod.id + ': ' + (mod.reason || 'reload blocked') +
                (mod.frame ? ' (' + mod.frame + ')' : '') + ' [reload blocked]');
        } else if (mod.status === 'failed' && failedChanged) {
            emit('error', 'failed ' + mod.id + ': ' + (mod.reason || 'unknown error') +
                (mod.frame ? ' (' + mod.frame + ')' : ''));
        } else if (mod.readyState === 'unconfirmed' && unconfirmedChanged) {
            emit('error', 'unconfirmed ' + mod.id + ': loaded but never reported ready');
        }
    }
    return { counts, readiness, degraded };
};


// --- P0 reporting helpers (JS-only; no Node APIs) ---
function nowMs() {
    try {
        if (typeof performance !== 'undefined' && performance && typeof performance.now === 'function') return performance.now();
        if (typeof Date !== 'undefined' && Date.now) return Date.now();
    } catch (_) { /* timing is best effort */ }
    return 0;
}

function elapsed(started) {
    const ms = nowMs() - started;
    return ms > 0 && ms < 3600000 ? Math.round(ms) : 0;
}

exports.report = function report(state) {
    try { if (Array.isArray(state.mods)) exports.markReadiness(state); } catch (_) { /* reporting must never throw */ }
    const snapshot = {
        version: 1,
        phase: state.lastPhase || 'startup',
        status: state.status,
        counts: exports.countMods(state.mods),
        readiness: state.readiness || { confirmed: 0, unconfirmed: 0, unverified: 0 },
        reloads: state.reloadCount || 0,
        mods: (state.mods || []).filter(mod => mod && !mod.retained).map(mod => ({
            id: mod.id,
            status: mod.status,
            entry: mod.entry || '',
            readyState: mod.status === 'loaded' ? (mod.readyState || 'unverified') : 'not-loaded',
            reloadError: mod.reloadError === true,
            loadMs: typeof mod.loadMs === 'number' ? mod.loadMs : 0,
            reloadMs: typeof mod.reloadMs === 'number' ? mod.reloadMs : 0,
            reason: mod.reason || '',
            frame: mod.frame || ''
        }))
    };
    return JSON.stringify(snapshot);
};
