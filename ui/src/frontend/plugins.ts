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

import type {PerfettoPlugin, PerfettoPluginStatic} from '../public/plugin';

// Helper for plugin "barrel" modules built on top of Vite's import.meta.glob.
//
// IMPORTANT: the import.meta.glob() call itself MUST live at the call site with
// a literal pattern and literal options — Vite statically analyses both at
// build time and cannot follow a pattern passed through a function. So the
// glob stays in the caller; this helper owns the two things that ARE shareable:
// the deterministic ordering rule and the type cast away from `unknown`.
//
// Callers must use these exact options so behaviour matches across barrels:
//
//   collectPluginBarrel(
//     import.meta.glob('../plugins/* /index.ts', {eager: true, import: 'default'}),
//   )
//
//   - eager:        synchronous, inlined imports (a barrel, not lazy chunks).
//   - import:'default': each plugin dir default-exports its PerfettoPluginStatic.
//
// Vite re-evaluates the glob when a matching dir is added or removed, so the
// barrel stays correct without any manual file-watching.
export function collectPluginBarrel(
  modules: Record<string, unknown>,
): PerfettoPluginStatic<PerfettoPlugin>[] {
  // import.meta.glob keys are the matched file paths. Sorting them yields a
  // stable, path-ordered plugin list (previously achieved by sorting dir names
  // in the vite.config synth-module plugin).
  return Object.keys(modules)
    .sort()
    .map((key) => modules[key] as PerfettoPluginStatic<PerfettoPlugin>);
}

// Barrel of every plugin and core plugin, one entry per sub-directory.
//
// Each plugin lives in its own dir with an index.ts default-exporting a
// PerfettoPluginStatic. Vite's import.meta.glob discovers them at build time
// and re-evaluates when a dir is added or removed, so there's nothing to keep
// in sync by hand. The literal pattern + options are required by Vite's static
// analysis; the ordering and cast are factored into collectPluginBarrel.

export const plugins = collectPluginBarrel(
  import.meta.glob('../plugins/*/index.ts', {eager: true, import: 'default'}),
);

export const corePlugins = collectPluginBarrel(
  import.meta.glob('../core_plugins/*/index.ts', {
    eager: true,
    import: 'default',
  }),
);
