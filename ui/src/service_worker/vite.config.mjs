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

import {defineConfig} from 'vite';
import path from 'node:path';
import {fileURLToPath} from 'node:url';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const ROOT_DIR = path.resolve(__dirname, '../../..');
const OUT_DIR = path.join(ROOT_DIR, 'ui/out/dist');

const NO_SOURCE_MAPS = process.env.NO_SOURCE_MAPS === 'true';

export default defineConfig({
  root: __dirname,
  publicDir: false,
  build: {
    outDir: OUT_DIR,
    emptyOutDir: false,
    sourcemap: !NO_SOURCE_MAPS,
    lib: {
      entry: path.resolve(__dirname, 'service_worker.ts'),
      formats: ['iife'],
      name: 'service_worker',
      fileName: () => 'service_worker.js',
    },
    rollupOptions: {
      output: {
        inlineDynamicImports: true,
      },
    },
  },
});
