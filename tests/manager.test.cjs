'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');
const base = path.resolve(__dirname, '../runtime/ProjectPBZ/Content/JavaScript/PBZModLoader');
const { load } = require(path.join(base, 'manager.js'));
const mod = (id, enabled = true, entry = './mods/' + id + '/main.js') => ({ id, enabled, entry });
test('ordered loading, disabled entries, failure isolation', () => {
    const calls = [], logs = [];
    const result = load({ version: 1, mods: [mod('a'), mod('off', false), mod('bad'), mod('b')] }, entry => {
        calls.push(entry); if (entry.includes('/bad/')) throw new Error('intentional'); return { entry };
    }, line => logs.push(line));
    assert.deepEqual(calls, ['./mods/a/main.js', './mods/bad/main.js', './mods/b/main.js']);
    assert.deepEqual(result.map(m => m.status), ['loaded', 'disabled', 'failed', 'loaded']);
    assert.match(logs[2], /intentional/);
});
test('invalid config rejected before any MOD executes', () => {
    for (const mods of [[mod('a'), mod('a')], [mod('A'), mod('a')], [mod('a', true, './mods/../escape.js')], [mod('a', true, './mods/b/main.js')], [mod('a', true, './mods/a/b/main.js')], [mod('a', true, './mods/a/other.js')], [mod('a', true, './generated-mods/a/main.js')], [mod('a', true, './mods/a/main.ts')], [mod('a', 'true')], [mod('a', true, './generated-reloads/x/generated-mods/a/main.js')], [mod('a', true, './mods/' + 'x'.repeat(520) + '/main.js')]]) {
        let calls = 0;
        assert.throws(() => load({version: 1, mods}, () => { calls++; }, () => {}));
        assert.equal(calls, 0);
    }
});
test('MOD count and ID limits reject oversized discovery data', () => {
    let calls = 0;
    const tooMany = Array.from({ length: 129 }, (_, i) => mod('m' + i));
    assert.throws(() => load({ version: 1, mods: tooMany }, () => { calls++; }, () => {}));
    assert.throws(() => load({ version: 1, mods: [mod('a'.repeat(65))] }, () => { calls++; }, () => {}));
    assert.equal(calls, 0);
});
test('bootstrap preserves original startup errors and loads only once', () => {
    const source = fs.readFileSync(path.join(base, 'bootstrap.js'), 'utf8');
    let original = 0, mods = 0;
    const context = vm.createContext({setInterval() {}, console: {log() {}, error() {}}, require(name) {
        if (name === 'MainStart') { original++; return {}; }
        if (name === './manager') return {load() { mods++; return []; }, watch() { return 1; }, createNotifier() { return () => {}; }};
        return {version: 1, mods: []};
    }});
    vm.runInContext(source, context); vm.runInContext(source, context);
    assert.equal(original, 2); assert.equal(mods, 1); assert.equal(context.__PBZModLoader.status, 'ready');
    const error = new Error('original startup');
    assert.throws(() => vm.runInNewContext(source, {require() {throw error;}}), value => value === error);
});

test('reload awaits cleanup before executing any new MOD and isolates cleanup failures', async () => {
    const { reload } = require(path.join(base, 'manager.js'));
    const calls = [];
    const old = [
        { id: 'a', status: 'loaded', instance: { async stop() { await Promise.resolve(); calls.push('stop a'); } } },
        { id: 'b', status: 'loaded', instance: { stop() { throw Error('cleanup'); } } },
        { id: 'removed', status: 'loaded', instance: { stop() { calls.push('stop removed'); } } }
    ];
    const result = await reload(old, { version: 1, mods: [mod('a'), mod('b')] }, entry => {
        calls.push(entry); return {};
    }, () => {});
    assert.deepEqual(calls, ['stop a', 'stop removed', './mods/a/main.js']);
    assert.equal(result[1], old[1]);
});

test('reload times out a hung stop() and keeps the old MOD blocked', async () => {
    const { reload } = require(path.join(base, 'manager.js'));
    const old = [{ id: 'a', status: 'loaded', instance: { stop() { return new Promise(() => {}); } } }];
    const calls = [];
    const result = await reload(old, { version: 1, mods: [mod('a')] }, entry => {
        calls.push(entry); return {};
    }, () => {}, 20);
    assert.deepEqual(calls, []);
    assert.equal(result[0], old[0]);
    assert.equal(old[0].reloadError, true);
    assert.equal(old[0].status, 'failed');
});

test('missing stop and failed evaluation cannot duplicate registrations; disabled MODs can load', async () => {
    const { reload } = require(path.join(base, 'manager.js'));
    const old = [{ id: 'a', status: 'loaded', instance: {} }, { id: 'bad', status: 'failed' }, { id: 'off', status: 'disabled' }];
    const calls = [];
    const result = await reload(old, { version: 1, mods: [mod('a'), mod('bad'), mod('off')] }, entry => { calls.push(entry); return {}; }, () => {});
    assert.deepEqual(calls, ['./mods/off/main.js']);
    assert.equal(result[0], old[0]);
    assert.equal(result[1], old[1]);
    await assert.rejects(reload(old, { version: 2, mods: [] }, () => {}, () => {}));
});

test('blocked failure is matched by MOD ID regardless of letter case', async () => {
    const { reload } = require(path.join(base, 'manager.js'));
    const old = [{ id: 'bad', status: 'failed', reason: 'original failure' }];
    const calls = [];
    const result = await reload(old, { version: 1, mods: [mod('BAD')] }, entry => { calls.push(entry); return {}; }, () => {});
    assert.deepEqual(calls, []);
    assert.equal(result[0], old[0]);
});

test('watch serializes slow stops, ignores missing revisions and uses fresh dependency paths', async () => {
    const { watch } = require(path.join(base, 'manager.js'));
    let tick, release, published = false, starts = 0, active = 1;
    const state = { mods: [{ id: 'a', status: 'loaded', instance: { stop: () => new Promise(resolve => { release = () => { active--; resolve(); }; }) } }] };
    watch(state, entry => {
        if (entry.endsWith('/config')) {
            if (!published || !entry.includes('/1/')) throw Error('missing');
            return { version: 1, mods: [mod('a', true, './generated-reloads/1/generated-mods/a/main.js')] };
        }
        assert.equal(entry, './generated-reloads/1/generated-mods/a/main.js');
        assert.equal(active, 0); active++; starts++;
        return { stop() { active--; } };
    }, () => {}, (callback, delay) => { tick = callback; assert.equal(delay, 1000); });
    await tick(); assert.equal(starts, 0);
    published = true;
    const pending = tick(); await tick(); assert.equal(starts, 0);
    release(); await pending; await tick();
    assert.equal(starts, 1); assert.equal(active, 1);
});

test('reload results distinguish blocked cleanup and preparation errors on screen', async () => {
    const { watch } = require(path.join(base, 'manager.js'));
    let tick;
    const notices = [];
    const state = { mods: [{ id: 'a', status: 'loaded', instance: {} }] };
    watch(state, entry => entry.includes('/1/') ? { version: 1, mods: [mod('a')] } : { error: 'compile failed' },
        () => {}, callback => { tick = callback; }, (message, failed) => notices.push({ message, failed }));
    await tick();
    assert.match(notices[0].message, /成功 0，失败或跳过 1/);
    assert.equal(notices[0].failed, true);
    const instance = state.mods[0];
    await tick();
    assert.match(notices[1].message, /重载失败/);
    assert.equal(state.mods[0], instance);
});

test('notification replaces the previous overlay, expires, and tolerates missing UI', () => {
    const { createNotifier } = require(path.join(base, 'manager.js'));
    const widgets = new Set(), timers = new Map(), logs = [];
    let serial = 0;
    const viewport = {
        AddWidget(widget, slot) { assert.equal(slot.ZOrder, 1000000); assert.equal(slot.Offsets.Left, 24); widgets.add(widget); return true; },
        RemoveWidget(widget) { widgets.delete(widget); }
    };
    class Struct {}
    const UE = {
        SubsystemBlueprintLibrary: { GetEngineSubsystem: () => viewport },
        GameViewportSubsystem: { StaticClass() {} },
        ESlateVisibility: { HitTestInvisible: 4 },
        KismetTextLibrary: { Conv_StringToText: text => ({ text }) },
        TextBlock: class {
            SetText(value) { assert.equal(typeof value, 'object'); this.text = value.text; }
            SetVisibility(value) { assert.equal(value, 4); }
            SetColorAndOpacity(color) { assert.equal(color.SpecifiedColor.A, 1); }
            SetShadowColorAndOpacity() {} SetShadowOffset() {}
        },
        SlateColor: Struct, LinearColor: Struct, Vector2D: Struct, Margin: Struct, GameViewportWidgetSlot: Struct
    };
    const notify = createNotifier(name => name === 'ue' ? UE : { argv: { getByName: () => ({}) } },
        message => logs.push(message), (callback, ms) => { assert.equal(ms, 4000); timers.set(++serial, callback); return serial; }, id => timers.delete(id));
    notify('success', false); notify('partial failure', true);
    assert.equal(widgets.size, 1); assert.equal(timers.size, 1);
    assert.match([...widgets][0].text, /partial failure/);
    [...timers.values()][0](); assert.equal(widgets.size, 0);
    UE.SubsystemBlueprintLibrary.GetEngineSubsystem = () => null;
    assert.doesNotThrow(() => notify('unavailable', true));
    assert.match(logs[0], /notification unavailable/);
});

test('notifier drops a widget rejected by the viewport instead of keeping a stale handle', () => {
    const { createNotifier } = require(path.join(base, 'manager.js'));
    const widgets = new Set();
    let reject = true;
    const viewport = {
        AddWidget(widget) { if (reject) return false; widgets.add(widget); return true; },
        RemoveWidget(widget) { widgets.delete(widget); }
    };
    class Struct {}
    const UE = {
        SubsystemBlueprintLibrary: { GetEngineSubsystem: () => viewport },
        GameViewportSubsystem: { StaticClass() {} },
        ESlateVisibility: { HitTestInvisible: 4 },
        KismetTextLibrary: { Conv_StringToText: text => ({ text }) },
        TextBlock: class { SetText() {} SetVisibility() {} SetColorAndOpacity() {} SetShadowColorAndOpacity() {} SetShadowOffset() {} },
        SlateColor: Struct, LinearColor: Struct, Vector2D: Struct, Margin: Struct, GameViewportWidgetSlot: Struct
    };
    const logs = [];
    const notify = createNotifier(name => name === 'ue' ? UE : { argv: { getByName: () => ({}) } },
        message => logs.push(message), () => 1, () => {});
    assert.doesNotThrow(() => notify('rejected', true));
    assert.equal(widgets.size, 0);
    assert.match(logs[0], /notification unavailable/);
    reject = false;
    notify('after rejection', false);
    assert.equal(widgets.size, 1);
});

test('load records actionable failure details, disabled reasons and per-MOD timing', () => {
    const failure = new Error('requires Mods Menu 0.5.1 native loader after MainStart');
    failure.stack = 'Error: requires Mods Menu 0.5.1 native loader after MainStart\n    at Object.main (mods\\BadMod\\main.js:8:53)\n    at require (internal)';
    const result = load({ version: 1, mods: [mod('Good'), mod('Bad'), mod('Off', false)] }, entry => {
        if (entry.includes('/Bad/')) throw failure;
        return { entry };
    }, () => {});
    const good = result[0], bad = result[1], off = result[2];
    assert.equal(good.status, 'loaded');
    assert.equal(good.entry, './mods/Good/main.js');
    assert.equal(typeof good.loadMs, 'number');
    assert.equal(bad.status, 'failed');
    assert.equal(bad.reason, 'requires Mods Menu 0.5.1 native loader after MainStart');
    assert.match(bad.frame, /main\.js:8:53/);
    assert.equal(off.status, 'disabled');
    assert.match(off.reason, /disabled in configuration/);
});
test('readiness distinguishes confirmed, unconfirmed and unverified MODs', () => {
    const manager = require(path.join(base, 'manager.js'));
    const state = {
        mods: [
            { id: 'ready', status: 'loaded', instance: { ready: true } },
            { id: 'never', status: 'loaded', instance: {} },
            { id: 'lazy', status: 'loaded', instance: { ready: false } },
            { id: 'off', status: 'disabled' }
        ],
        readyIds: new Set()
    };
    assert.deepEqual(manager.markReadiness(state), { confirmed: 1, unconfirmed: 1, unverified: 1 });
    state.readyIds.add('lazy');
    assert.deepEqual(manager.markReadiness(state), { confirmed: 2, unconfirmed: 0, unverified: 1 });
    assert.equal(state.mods[1].readyState, 'unverified');
    assert.equal(state.mods[3].readyState, undefined);
});
test('status lines are deduplicated, slowest timing is shown and report JSON is complete', () => {
    const manager = require(path.join(base, 'manager.js'));
    const warnings = [], errors = [];
    const originalWarn = console.warn, originalError = console.error;
    console.warn = line => warnings.push(String(line));
    console.error = line => errors.push(String(line));
    try {
        const state = {
            mods: [
                { id: 'A', status: 'loaded', entry: './mods/A/main.js', loadMs: 120, instance: {} },
                { id: 'B', status: 'loaded', entry: './mods/B/main.js', loadMs: 5, instance: {} },
                { id: 'C', status: 'failed', entry: './mods/C/main.js', reason: 'boom', frame: 'mods/C/main.js:1:1' }
            ]
        };
        manager.markReadiness(state);
        manager.reportStatus(state, 'startup');
        manager.reportStatus(state, 'readiness-1s');
        assert.equal(warnings.length, 1, 'identical status line must not repeat');
        assert.match(warnings[0], /slowest=A:120ms/);
        assert.match(warnings[0], /failed=1/);
        assert.equal(errors.length, 1, 'failure detail is printed once');
        state.mods[2].status = 'loaded';
        manager.reportStatus(state, 'readiness-3s');
        assert.equal(warnings.length, 2, 'changed counters must print again');
        const report = JSON.parse(manager.report(state));
        assert.equal(report.counts.loaded, 3);
        assert.equal(report.mods.length, 3);
        assert.equal(report.mods[0].loadMs, 120);
        assert.deepEqual(report.readiness, { confirmed: 0, unconfirmed: 0, unverified: 3 });
        assert.equal(report.phase, 'readiness-3s');
    } finally {
        console.warn = originalWarn;
        console.error = originalError;
    }
});
test('bootstrap exposes declarative readiness and a JSON report', () => {
    const source = fs.readFileSync(path.join(base, 'bootstrap.js'), 'utf8');
    const manager = require(path.join(base, 'manager.js'));
    const warnings = [], errors = [], logged = [];
    const originalWarn = console.warn, originalError = console.error;
    console.warn = line => warnings.push(String(line));
    console.error = line => errors.push(String(line));
    try {
        const context = vm.createContext({
            setInterval() { return 1; },
            console: { log: line => logged.push(String(line)), error: line => errors.push(String(line)) },
            require(name) {
                if (name === 'MainStart') return {};
                if (name === './manager') return manager;
                if (name === './generated-config') return { version: 1, mods: [mod('ReadyMod')] };
                if (name === './mods/ReadyMod/main.js') return { ready: true };
                throw new Error('unexpected import ' + name);
            }
        });
        vm.runInContext(source, context);
        assert.equal(context.__PBZModLoader.status, 'ready');
        context.__PBZDeclareReady('ReadyMod');
        manager.markReadiness(context.__PBZModLoader);
        assert.equal(context.__PBZModLoader.readiness.confirmed, 1);
        const report = JSON.parse(context.__PBZModLoaderReport());
        assert.equal(report.mods.length, 1);
        assert.equal(report.counts.loaded, 1);
        assert.equal(report.readiness.confirmed, 1);
        assert.ok(logged.concat(warnings).some(line => /status phase=startup loaded=1/.test(line)));
    } finally {
        console.warn = originalWarn;
        console.error = originalError;
    }
});

test('startup status line includes the readiness computed from loaded MODs', () => {
    const source = fs.readFileSync(path.join(base, 'bootstrap.js'), 'utf8');
    const warnings = [], originalWarn = console.warn;
    console.warn = line => warnings.push(String(line));
    try {
        const context = vm.createContext({
            setInterval() { return 1; },
            console: { log() {}, error() {} },
            require(name) {
                if (name === 'MainStart') return {};
                if (name === './manager') return require(path.join(base, 'manager.js'));
                if (name === './generated-config') return { version: 1, mods: [mod('Plain')] };
                if (name === './mods/Plain/main.js') return {};
                throw new Error('unexpected import ' + name);
            }
        });
        vm.runInContext(source, context);
        const startup = warnings.find(line => /status phase=startup/.test(line));
        assert.ok(startup, 'startup status line must be printed');
        assert.match(startup, /unverified=1/);
        assert.match(startup, /blocked=0/);
        assert.match(startup, /degraded=0/);
    } finally {
        console.warn = originalWarn;
    }
});

test('blocked reload is visible in the status line, detail line and report', () => {
    const manager = require(path.join(base, 'manager.js'));
    const warnings = [], errors = [];
    const originalWarn = console.warn, originalError = console.error;
    console.warn = line => warnings.push(String(line));
    console.error = line => errors.push(String(line));
    try {
        const state = { mods: [{
            id: 'A', status: 'loaded', entry: './mods/A/main.js', instance: {},
            reason: 'cleanup boom', frame: 'mods/A/main.js:1:1', reloadError: true
        }] };
        manager.markReadiness(state);
        manager.reportStatus(state, 'reload');
        const line = warnings.find(text => /status phase=reload/.test(text));
        assert.ok(line);
        assert.match(line, /blocked=1/);
        assert.match(line, /degraded=1/);
        assert.ok(errors.some(text => /blocked A: cleanup boom/.test(text) && /\[reload blocked\]/.test(text)));
        const report = JSON.parse(manager.report(state));
        assert.equal(report.counts.blocked, 1);
        assert.equal(report.mods[0].reloadError, true);
    } finally {
        console.warn = originalWarn;
        console.error = originalError;
    }
});

test('reload disables a previously failed MOD and hides removed evaluation failures', async () => {
    const manager = require(path.join(base, 'manager.js'));
    const failed = { id: 'Bad', status: 'failed', entry: './mods/Bad/main.js', reason: 'boom' };
    const disabled = await manager.reload([failed], { version: 1, mods: [mod('Bad', false)] },
        () => { throw new Error('must not import a disabled MOD'); }, () => {});
    assert.equal(disabled[0].status, 'disabled');
    const hidden = disabled.find(entry => entry.retained === true);
    assert.ok(hidden && hidden.status === 'failed');
    let imports = 0;
    const removed = await manager.reload([hidden], { version: 1, mods: [] }, () => { imports++; }, () => {});
    assert.equal(imports, 0);
    assert.ok(removed[0].retained === true);
    const reenabled = await manager.reload(removed, { version: 1, mods: [mod('Bad')] },
        () => { throw new Error('failed MOD must not be retried'); }, () => {});
    assert.equal(reenabled[0].status, 'failed');
    assert.equal(reenabled.length, 1);
});

test('blocked reload keeps the readiness declaration of the still-running instance', async () => {
    const manager = require(path.join(base, 'manager.js'));
    const instance = {};
    const state = { mods: [{ id: 'A', status: 'loaded', entry: './mods/A/main.js', instance }], readyIds: new Set(['a']) };
    let tick;
    manager.watch(state, name => {
        if (name.endsWith('/config')) return { version: 1, mods: [mod('A')] };
        throw new Error('unexpected import ' + name);
    }, () => {}, callback => { tick = callback; }, () => {});
    await tick();
    assert.equal(state.mods[0].instance, instance);
    assert.equal(state.mods[0].reloadError, true);
    const readiness = manager.markReadiness(state);
    assert.equal(readiness.confirmed, 1);
});

test('report marks non-loaded MODs as not-loaded and clears stale readiness', () => {
    const manager = require(path.join(base, 'manager.js'));
    const state = { mods: [
        { id: 'A', status: 'failed', instance: {}, readyState: 'confirmed', reason: 'boom' },
        { id: 'B', status: 'disabled', readyState: 'confirmed' },
        { id: 'C', status: 'stopped' }
    ] };
    manager.markReadiness(state);
    assert.equal(state.mods[0].readyState, undefined);
    assert.equal(state.mods[1].readyState, undefined);
    const report = JSON.parse(manager.report(state));
    assert.equal(report.mods[0].readyState, 'not-loaded');
    assert.equal(report.mods[1].readyState, 'not-loaded');
    assert.equal(report.mods[0].reloadError, false);
});

