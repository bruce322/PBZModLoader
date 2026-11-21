// Fixed, reviewed export manifests; never depend on the build host's System32.
const fs = require('node:fs');
const path = require('node:path');
const root = path.resolve(__dirname, '..');
const names = ['version', 'hid', 'winmm', 'dsound', 'dxgi'];
const asm = fs.readFileSync(path.join(root, 'src/proxy.S'), 'utf8').split('    version_export 0')[0];
if (!asm || asm === fs.readFileSync(path.join(root, 'src/proxy.S'), 'utf8'))
  throw Error('src/proxy.S is missing the version_export placeholder');
for (const name of names) {
  const def = fs.readFileSync(path.join(root, name === 'version' ? 'src/version.def' : `src/proxies/${name}.def`), 'utf8');
  const exports = [...def.matchAll(/^[ \t]+(\w+)=pbz_version_(\d+) @(\d+)( NONAME)?[ \t\r]*$/gm)]
    .map(m => ({name: m[1], index: +m[2], ordinal: +m[3], noname: !!m[4]}));
  if (!exports.length || exports.some((e, i) => e.index !== i) ||
      new Set(exports.map(e => e.ordinal)).size !== exports.length ||
      new Set(exports.map(e => e.name)).size !== exports.length)
    throw Error(`Invalid export manifest: ${name}`);
  const dir = path.join(root, 'build/proxies', name);
  fs.mkdirSync(dir, {recursive: true});
  fs.writeFileSync(path.join(dir, 'proxy_config.hpp'),
    `#pragma once\n#define PBZ_PROXY_DLL L"\\\\${name}.dll"\n` +
    `static const char* const versionNames[]={${exports.map(e => e.noname ? `(const char*)${e.ordinal}` : JSON.stringify(e.name)).join(',')}};\n` +
    `static constexpr unsigned proxyCount=${exports.length};\n`);
  fs.writeFileSync(path.join(dir, 'proxy.S'), asm + exports.map(e => `    version_export ${e.index}\n`).join(''));
  fs.writeFileSync(path.join(dir, 'exports.json'), JSON.stringify(exports, null, 2));
}
console.log('Generated five proxy configurations from fixed export manifests.');
