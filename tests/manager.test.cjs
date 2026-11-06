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
    for (const mods of [[mod('a'), mod('a')], [mod('A'), mod('a')], [mod('a', true, './mods/../escape.js')], [mod('a', true, './mods/b/main.js')], [mod('a', true, './mods/a/b/main.js')], [mod('a', true, './generated-mods/a/main.js')], [mod('a', true, './mods/a/main.ts')], [mod('a', 'true')], [mod('a', true, './generated-reloads/x/generated-mods/a/main.js')], [mod('a', true, './mods/' + 'x'.repeat(520) + '/main.js')]]) {
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
