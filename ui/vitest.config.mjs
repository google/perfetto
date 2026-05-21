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

// Standalone config for Vitest. We don't reuse vite.config.mjs because that
// file requires a BUNDLE env var at module-load time (one bundle per build
// process). Tests don't bundle, so we declare only what the test runtime
// needs.

import {defineConfig} from 'vitest/config';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {lezer} from '@lezer/generator/rollup';
import checker from 'vite-plugin-checker';
import {pluginGenRelativeImports} from './vite/gen.mjs';
import {pluginPerfettoPluginBarrels} from './vite/plugins.mjs';
import {pluginPerfettoVersion} from './vite/version.mjs';
import {pluginPerfettoVirtualWasmModules} from './vite/wasm.mjs';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const ROOT_DIR = path.dirname(__dirname);
const SRC = path.join(ROOT_DIR, 'ui/src');

export default defineConfig({
  // Pin root to ui/ (where package.json and node_modules live). Without this,
  // Vitest uses cwd — which is the build's out dir when invoked from build.mjs,
  // so test discovery fails and the cache lands in the wrong place.
  root: __dirname,
  plugins: [
    // *.grammar imports used by codemirror plugins.
    lezer(),
    // Virtual modules consumed by ui/src/ (kept in sync with vite.config.mjs).
    pluginPerfettoVersion({
      virtualModule: path.join(SRC, 'virtual', 'version'),
      scriptPath: path.join(ROOT_DIR, 'tools/write_version_header.py'),
    }),
    pluginPerfettoPluginBarrels({
      sources: [
        {exportName: 'plugins', dir: path.join(SRC, 'plugins'), prefix: ''},
        {
          exportName: 'corePlugins',
          dir: path.join(SRC, 'core_plugins'),
          prefix: 'core_',
        },
      ],
      virtualModule: path.join(SRC, 'virtual', 'plugins'),
    }),
    pluginPerfettoVirtualWasmModules({
      virtualDir: path.join(SRC, 'virtual'),
      genDir: path.join(SRC, 'gen'),
    }),
    // pluginGenRelativeImports({genSymlink: path.join(SRC, 'gen')}),
    // Run tsc in-process so type errors fail the test run alongside
    // assertion failures.
    checker({typescript: true, overlay: false}),
  ],
  define: {
    'process.env.NODE_ENV': JSON.stringify('test'),
  },
  test: {
    environment: 'happy-dom',
    globals: true,
    include: ['src/**/*_unittest.ts', 'src/**/*_jsdomtest.ts'],
  },
});
