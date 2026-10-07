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

// Standalone config for Vitest. We don't reuse vite.config.mjs wholesale
// because that file requires a BUNDLE env var when building (one bundle per
// build process). Tests don't bundle, so we declare only what the test
// runtime needs, importing the shared synth-module plugins from it.

import {defineConfig} from 'vitest/config';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {lezer} from '@lezer/generator/rollup';
import {pluginPerfettoVersion} from './vite.config.mjs';

const __dirname = path.dirname(fileURLToPath(import.meta.url));

export default defineConfig({
  // Pin root to ui/ (where package.json and node_modules live). Without this,
  // Vitest uses cwd — which is the build's out dir when invoked from build.mjs,
  // so test discovery fails and the cache lands in the wrong place.
  root: __dirname,
  plugins: [
    // *.grammar imports used by codemirror plugins.
    lezer(),
    // Synthesised modules under ui/src/virtual.
    pluginPerfettoVersion(),
  ],
  define: {
    'process.env.NODE_ENV': JSON.stringify('test'),
  },
  test: {
    environment: 'jsdom',
    globals: true,
    include: ['src/**/*_unittest.ts', 'src/**/*_jsdomtest.ts'],
    // Registers @testing-library/jest-dom matchers (toBeInTheDocument, etc.).
    setupFiles: ['vitest.setup.ts'],
  },
});
