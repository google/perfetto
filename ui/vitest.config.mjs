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

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const ROOT_DIR = path.dirname(__dirname);
const SRC = path.join(ROOT_DIR, 'ui/src');

export default defineConfig({
  root: SRC,
  plugins: [
    // *.grammar imports used by codemirror plugins.
    lezer(),
  ],
  define: {
    'process.env.NODE_ENV': JSON.stringify('test'),
  },
  test: {
    environment: 'jsdom',
    globals: true,
    include: ['**/*_unittest.ts', '**/*_jsdomtest.ts'],
    setupFiles: [path.join(__dirname, 'config/vitest_setup.ts')],
  },
});
