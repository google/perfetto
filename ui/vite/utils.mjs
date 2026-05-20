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

// Shared helper for plugins that synthesise a module's source on the fly but
// expose it via a normal relative import (typed by a colocated .d.ts).
//
// `modules` maps an absolute path (no extension) — e.g. <SRC>/plugins/index —
// to a function that returns the module source. resolveId intercepts both
// file-style imports ('../base/version') and directory-style imports
// ('../plugins' → '../plugins/index') before Vite's filesystem resolver runs.
export function makeSynthModulePlugin({name, modules}) {
  const PREFIX = '\0' + name + ':';
  return {
    name,
    enforce: 'pre',
    resolveId(source, importer) {
      if (!importer || !source.startsWith('.')) return null;
      const stripped = source.replace(/\.(ts|js)$/, '');
      const abs = path.resolve(path.dirname(importer), stripped);
      for (const candidate of [abs, path.join(abs, 'index')]) {
        if (candidate in modules) return PREFIX + candidate;
      }
    },
    load(id) {
      if (!id.startsWith(PREFIX)) return;
      const gen = modules[id.slice(PREFIX.length)];
      if (gen) return gen(this);
    },
  };
}
