'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const vm = require('node:vm');
const { spawnSync } = require('node:child_process');
const { load } = require('../runtime/ProjectPBZ/Content/JavaScript/PBZModLoader/manager.js');
const template = path.resolve(__dirname, '../templates/ts-mod');
const compiler = path.join(path.dirname(require.resolve('typescript/package.json')), 'bin/tsc');

test('TS template compiles and works with the current CommonJS manager', async t => {
    const scratch = fs.mkdtempSync(path.join(os.tmpdir(), 'pbz template '));
    t.after(() => fs.rmSync(scratch, { recursive: true, force: true }));
    for (const entry of ['src', 'types', 'vendor', 'tsconfig.json']) {
        fs.cpSync(path.join(template, entry), path.join(scratch, entry), { recursive: true });
    }
    // Same package resolution as npm's local dependency, without network use in unit tests.
    fs.mkdirSync(path.join(scratch, 'node_modules/@pbz-mod-loader'), { recursive: true });
    fs.cpSync(path.join(scratch, 'vendor/game-api'), path.join(scratch, 'node_modules/@pbz-mod-loader/game-api'), { recursive: true });
    function compile(...args) {
        const result = spawnSync(process.execPath, [compiler, '-p', path.join(scratch, 'tsconfig.json'), ...args], { encoding: 'utf8' });
        assert.ifError(result.error);
        assert.equal(result.status, 0, result.stdout + result.stderr);
    }
    compile();
    const source = fs.readFileSync(path.join(scratch, 'dist/my-mod/main.js'), 'utf8');

    function launch({ missing = false } = {}) {
        const timers = new Map(), logs = [];
        let failMap = false;
        const exports = {};
        const context = vm.createContext({
            exports,
            console: Object.fromEntries(['log', 'warn', 'error'].map(level => [level, message => logs.push(message)])),
            setInterval(callback, milliseconds) {
                assert.equal(milliseconds, 2000);
                timers.set(1, callback);
                return 1;
            },
            clearInterval(handle) { timers.delete(handle); },
            require(name) {
                if (name === 'puerts') return { argv: { getByName(key) {
                    assert.equal(key, 'GameInstance');
                    return missing ? undefined : { GetName: () => 'TestGameInstance' };
                } } };
                if (name === 'ue') return { GameplayStatics: { GetCurrentLevelName(instance, removePrefix) {
                    assert.equal(instance.GetName(), 'TestGameInstance');
                    assert.equal(removePrefix, true);
                    if (failMap) throw new Error('world unavailable');
                    return 'TestMap';
                } } };
                throw new Error('Unexpected runtime dependency: ' + name);
            }
        });
        const result = load({ version: 1, mods: [{ id: 'my-mod', enabled: true, entry: './mods/my-mod/main.js' }] }, entry => {
            assert.equal(entry, './mods/my-mod/main.js');
            vm.runInContext(source, context);
            return exports;
        }, line => logs.push(line));
        return { result, timers, logs, failMap() { failMap = true; } };
    }

    await t.test('five samples then timer cleanup; exported stop is repeatable', () => {
        const run = launch();
        assert.equal(run.result[0].status, 'loaded');
        for (let i = 0; i < 4; i++) run.timers.get(1)();
        assert.equal(run.timers.size, 0);
        assert.equal(run.logs.filter(line => line.includes('GameInstance=TestGameInstance map=TestMap')).length, 5);
        run.result[0].instance.stop();
        run.result[0].instance.stop();
        assert.equal(run.logs.filter(line => line.endsWith('stopped')).length, 1);
    });

    await t.test('missing initial GameInstance fails loading without leaking a timer', () => {
        const run = launch({ missing: true });
        assert.equal(run.result[0].status, 'failed');
        assert.equal(run.timers.size, 0);
        assert.ok(run.logs.some(line => line.includes('GameInstance missing')));
    });

    await t.test('timer callback errors and manual stop both release the timer', () => {
        const run = launch();
        run.failMap();
        run.timers.get(1)();
        assert.equal(run.timers.size, 0);
        assert.ok(run.logs.some(line => line.includes('world unavailable')));
        const manual = launch();
        manual.result[0].instance.stop();
        assert.equal(manual.timers.size, 0);
    });

    await t.test('type checking rejects incorrect API calls and unavailable host globals', () => {
        fs.writeFileSync(path.join(scratch, 'src/type-probe.ts'), `
import { argv } from 'puerts';
import { GameplayStatics } from 'ue';
// @ts-expect-error An object is required, not a number.
GameplayStatics.GetCurrentLevelName(123, true);
// @ts-expect-error Misspelled argument names must not silently become any.
argv.getByName('GameInstnace');
// @ts-expect-error Node globals are not part of the game API.
process.cwd();
// @ts-expect-error Browser globals are not part of the game API.
window.document;
// @ts-expect-error Undeclared modules must not bypass checking via require.
require('fs');
`);
        compile('--noEmit');
    });
});
