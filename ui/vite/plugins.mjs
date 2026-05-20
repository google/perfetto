// Copyright (C) 2026 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

import path from 'node:path';
import fs from 'node:fs';
import {makeSynthModulePlugin} from './utils.mjs';

// Synthesises a barrel virtual module that imports every sub-directory under
// each |source| dir and exposes them as named arrays. Each source provides:
//   - exportName: name of the exported array
//   - dir:        absolute directory to scan
//   - prefix:     prefix added to each entry's local binding name
// |virtualModule| is the absolute path (no extension) the barrel resolves to.
export function pluginPerfettoPluginBarrels({sources, virtualModule}) {
  const pluginDirs = sources.map((s) => s.dir);
  const toCamelCase = (s) => {
    const [first, ...rest] = s.split(/[._]/);
    return (
      first + rest.map((x) => x.charAt(0).toUpperCase() + x.slice(1)).join('')
    );
  };
  const listEntries = (dir) =>
    fs
      .readdirSync(dir)
      .map((name) => ({name, full: path.join(dir, name)}))
      .filter(({full}) => {
        try {
          return (
            fs.statSync(full).isDirectory() &&
            fs.existsSync(path.join(full, 'index.ts'))
          );
        } catch (_) {
          return false;
        }
      })
      .sort((a, b) => a.name.localeCompare(b.name));
  const generate = (ctx) => {
    const importLines = [];
    const exportLines = [];
    for (const {exportName, dir, prefix} of sources) {
      const entries = listEntries(dir);
      for (const {full} of entries) {
        ctx.addWatchFile(path.join(full, 'index.ts'));
      }
      for (const {name, full} of entries) {
        importLines.push(
          `import ${toCamelCase(prefix + name)} from '${full}';`,
        );
      }
      const arr = entries
        .map(({name}) => `  ${toCamelCase(prefix + name)},`)
        .join('\n');
      exportLines.push(`export const ${exportName} = [\n${arr}\n];`);
    }
    return `${importLines.join('\n')}\n\n${exportLines.join('\n\n')}\n`;
  };
  const base = makeSynthModulePlugin({
    name: 'perfetto:plugin-barrels',
    modules: {[virtualModule]: generate},
  });
  let server = null;
  return {
    ...base,
    configureServer(s) {
      server = s;
      // Watch the parent dirs so adding/removing a plugin dir invalidates
      // the barrel even before any file inside it changes.
      for (const dir of pluginDirs) s.watcher.add(dir);
    },
    handleHotUpdate(ctx) {
      if (!server) return;
      for (const dir of pluginDirs) {
        if (!ctx.file.startsWith(dir + path.sep)) continue;
        const id = '\0perfetto:plugin-barrels:' + virtualModule;
        const mod = server.moduleGraph.getModuleById(id);
        if (mod) server.moduleGraph.invalidateModule(mod);
        return;
      }
    },
  };
}
