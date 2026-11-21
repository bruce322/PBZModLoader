'use strict';
// Original startup exceptions must retain their original propagation behavior.
console.log('[PBZMOD] bootstrap entered');
try {
    require('MainStart');
} catch (error) {
    console.error('[PBZMOD] original MainStart failed: ' + String(error.stack || error));
    throw error;
}
console.log('[PBZMOD] original MainStart returned');
if (!globalThis.__PBZModLoader) {
    const state = globalThis.__PBZModLoader = {
        version: 1, mods: [], status: 'loading',
        readiness: { confirmed: 0, unconfirmed: 0, unverified: 0 },
        readyIds: new Set(), readyTimers: [],
        lastPhase: 'startup', statusSignature: '', lastFailedIds: '', lastBlockedIds: '', lastUnconfirmedIds: '', reloadCount: 0
    };
    globalThis.__PBZDeclareReady = function (id) {
        try { state.readyIds.add(String(id).toLowerCase()); } catch (_) { /* ignore bad ids */ }
    };
    globalThis.__PBZModLoaderReport = function () {
        try {
            const manager = require('./manager');
            const report = typeof manager.report === 'function' ? manager.report(state) : '';
            try { console.log('[PBZMOD] report ' + report); } catch (_) { /* console is optional */ }
            return report;
        } catch (error) {
            try { console.error('[PBZMOD] report failed: ' + String(error && error.stack || error)); } catch (_) { /* ignore */ }
            return '';
        }
    };
    try {
        const manager = require('./manager');
        const log = message => console.log('[PBZMOD] ' + message);
        state.mods = manager.load(require('./generated-config'), require, log);
        state.status = 'ready';
        if (typeof manager.markReadiness === 'function') manager.markReadiness(state);
        if (typeof manager.reportStatus === 'function') {
            try { manager.reportStatus(state, 'startup'); } catch (error) {
                try { console.error('[PBZMOD] startup status failed: ' + String(error && error.stack || error)); } catch (_) { /* ignore */ }
            }
        }
        function scheduleReadiness() {
            try {
                if (typeof setTimeout !== 'function') return;
                while (state.readyTimers.length) {
                    const pending = state.readyTimers.pop();
                    if (typeof clearTimeout === 'function') clearTimeout(pending);
                }
                const check = phase => {
                    try {
                        if (typeof manager.markReadiness === 'function') manager.markReadiness(state);
                        if (typeof manager.reportStatus === 'function') manager.reportStatus(state, phase);
                    } catch (error) {
                        try { console.error('[PBZMOD] readiness check failed: ' + String(error && error.stack || error)); } catch (_) { /* ignore */ }
                    }
                };
                state.readyTimers.push(setTimeout(() => check('readiness-1s'), 1200));
                state.readyTimers.push(setTimeout(() => check('readiness-3s'), 3000));
            } catch (error) {
                try { console.error('[PBZMOD] readiness scheduling failed: ' + String(error && error.stack || error)); } catch (_) { /* ignore */ }
            }
        }
        const notifier = manager.createNotifier(require, log);
        const notify = (message, failed) => {
            notifier(message, failed);
            state.reloadCount = (state.reloadCount || 0) + 1;
            if (typeof manager.markReadiness === 'function') {
                try { manager.markReadiness(state); } catch (error) {
                    try { console.error('[PBZMOD] readiness after reload failed: ' + String(error && error.stack || error)); } catch (_) { /* console is optional */ }
                }
            }
            if (typeof manager.reportStatus === 'function') {
                try { manager.reportStatus(state, 'reload'); } catch (error) {
                    try { console.error('[PBZMOD] reload status failed: ' + String(error && error.stack || error)); } catch (_) { /* ignore */ }
                }
            }
            scheduleReadiness();
        };
        state.reloadTimer = manager.watch(state, require, log, typeof setInterval === 'function' ? setInterval : undefined, notify);
        scheduleReadiness();
        console.log('[PBZMOD] ready loaded=' + state.mods.filter(m => m.status === 'loaded').length +
            ' failed=' + state.mods.filter(m => m.status === 'failed').length);
    } catch (error) {
        state.status = 'failed';
        console.error('[PBZMOD] configuration failed: ' + String(error.stack || error));
    }
}
