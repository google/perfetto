// Copyright (C) 2024 The Android Open Source Project
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

import {defineConfig, PluginOption} from 'vite';
import {resolve} from 'path';
import commonjs from '@rollup/plugin-commonjs';

import {
  devStaticServePlugin,
  lezerGrammarPlugin,
  perfettoVersionPlugin,
  wasmModulesPlugin,
} from './vite-plugins';

export default defineConfig(({command}) => ({
  // Only set root for dev server, not for build (build uses lib entries)
  root: command === 'serve' ? resolve(__dirname, 'src') : undefined,

  plugins: [
    devStaticServePlugin(),
    lezerGrammarPlugin(),
    perfettoVersionPlugin(),
    wasmModulesPlugin(),
    // Handle CommonJS modules - only for build
    ...(command === 'build'
      ? [
          commonjs({
            include: ['**/out/**', '**/node_modules/**'],
          }) as PluginOption,
        ]
      : []),
  ],

  // Pre-bundle CommonJS deps for dev server
  optimizeDeps: {
    include: ['mithril', 'd3', 'protobufjs', 'protobufjs/minimal'],
    esbuildOptions: {
      format: 'esm',
    },
  },

  resolve: {
    alias: {
      // Allow imports like 'src/base/foo' to work
      src: resolve(__dirname, 'src'),
      // Use Vite-compatible generated files instead of the old build's gen/ symlink
      '../gen/all_plugins': resolve(__dirname, 'src/gen_vite/all_plugins.ts'),
      '../gen/all_core_plugins': resolve(
        __dirname,
        'src/gen_vite/all_core_plugins.ts',
      ),
      // WASM module wrappers - these wrap the CommonJS Emscripten output as ESM
      '../gen/trace_processor_memory64': resolve(
        __dirname,
        'src/gen_vite/trace_processor_memory64.ts',
      ),
      '../gen/proto_utils': resolve(__dirname, 'src/gen_vite/proto_utils.ts'),
      '../gen/traceconv': resolve(__dirname, 'src/gen_vite/traceconv.ts'),
    },
    extensions: ['.mjs', '.mts', '.ts', '.tsx', '.js', '.jsx', '.json'],
  },

  build: {
    // Output to dist_version symlink (points to dist/v1.2.3/)
    outDir: resolve(__dirname, '../out/ui/ui/dist_version'),
    emptyOutDir: false,
    sourcemap: true,
    lib: {
      entry: {
        frontend_bundle: resolve(__dirname, 'src/frontend/index.ts'),
        engine_bundle: resolve(__dirname, 'src/engine/index.ts'),
        traceconv_bundle: resolve(__dirname, 'src/traceconv/index.ts'),
      },
      formats: ['es'],
      fileName: (_format, entryName) => `${entryName}.js`,
    },
    rollupOptions: {
      input: {
        frontend_bundle: resolve(__dirname, 'src/frontend/index.ts'),
        engine_bundle: resolve(__dirname, 'src/engine/index.ts'),
        traceconv_bundle: resolve(__dirname, 'src/traceconv/index.ts'),
      },
      output: {},
    },
  },

  worker: {
    format: 'es',
    plugins: () => [
      commonjs({
        include: ['**/out/**', '**/node_modules/**'],
      }) as PluginOption,
    ],
  },

  css: {
    preprocessorOptions: {
      scss: {},
    },
  },

  server: {
    port: 10000,
    strictPort: true,
    host: '127.0.0.1',
    fs: {
      allow: [resolve(__dirname, '..')],
    },
  },
}));
