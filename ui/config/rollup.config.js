// Copyright (C) 2018 The Android Open Source Project
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

import commonjs from '@rollup/plugin-commonjs';
import nodeResolve from '@rollup/plugin-node-resolve';
import replace from 'rollup-plugin-re';
import sourcemaps from 'rollup-plugin-sourcemaps';

const path = require('path');
const ROOT_DIR = path.dirname(path.dirname(__dirname));  // The repo root.
const OUT_SYMLINK = path.join(ROOT_DIR, 'ui/out');

function defBundle(bundle, distDir) {
  return {
    input: `${OUT_SYMLINK}/tsc/${bundle}/index.js`,
    output: {
      name: bundle,
      format: 'iife',
      esModule: false,
      file: `${OUT_SYMLINK}/${distDir}/${bundle}_bundle.js`,
      sourcemap: true,
    },
    plugins: [
      nodeResolve({
        mainFields: ['browser'],
        browser: true,
        preferBuiltins: false,
      }),
      // emscripten conditionally executes require('fs') (likewise for
      // others), when running under node. Rollup can't find those libraries
      // so expects these to be present in the global scope, which then fails
      // at runtime. To avoid this we ignore require('fs') and the like.
      commonjs({
        ignore: [
          'fs',
          'path',
          'crypto',
        ],
      }),

      replace({
        patterns: [
          // Protobufjs's inquire() uses eval but that's not really needed in
          // the browser. https://github.com/protobufjs/protobuf.js/issues/593
          {test: /eval\(.*\(moduleName\);/g, replace: 'undefined;'},

          // Immer entry point has a if (process.env.NODE_ENV === 'production')
          // but |process| is not defined in the browser. Bypass.
          // https://github.com/immerjs/immer/issues/557
          {test: /process\.env\.NODE_ENV/g, replace: '\'production\''},
        ],
      }),

      // Translate source maps to point back to the .ts sources.
      sourcemaps(),
    ],
    onwarn: function(warning, warn) {
      // Ignore circular dependency warnings coming from third party code.
      if (warning.code === 'CIRCULAR_DEPENDENCY' &&
          warning.importer.includes('node_modules')) {
        return;
      }

      // Call the default warning handler for all remaining warnings.
      warn(warning);
    },
  };
}

function defServiceWorkerBundle() {
  return {
    input: `${OUT_SYMLINK}/tsc/service_worker/service_worker.js`,
    output: {
      name: 'service_worker',
      format: 'iife',
      esModule: false,
      file: `${OUT_SYMLINK}/dist/service_worker.js`,
      sourcemap: true,
    },
    plugins: [
      nodeResolve({
        mainFields: ['browser'],
        browser: true,
        preferBuiltins: false,
      }),
      commonjs(),
      sourcemaps(),
    ],
  };
}

export default [
  defBundle('frontend', 'dist_version'),
  defBundle('engine', 'dist_version'),
  defBundle('traceconv', 'dist_version'),
  defBundle('chrome_extension', 'chrome_extension'),
  defServiceWorkerBundle(),
];
