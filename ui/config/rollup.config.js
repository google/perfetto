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

const {uglify} = require('rollup-plugin-uglify');
const commonjs = require('@rollup/plugin-commonjs');
const nodeResolve = require('@rollup/plugin-node-resolve');
const path = require('path');
const replace = require('rollup-plugin-re');
const sourcemaps = require('rollup-plugin-sourcemaps');

const ROOT_DIR = path.dirname(path.dirname(__dirname)); // The repo root.
const OUT_SYMLINK = path.join(ROOT_DIR, 'ui/out');

function defBundle(tsRoot, bundle, distDir) {
  return {
    input: `${OUT_SYMLINK}/${tsRoot}/${bundle}/index.js`,
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

      commonjs({
        strictRequires: true,
      }),

      replace({
        patterns: [
          // Protobufjs's inquire() uses eval but that's not really needed in
          // the browser. https://github.com/protobufjs/protobuf.js/issues/593
          {test: /eval\(.*\(moduleName\);/g, replace: 'undefined;'},

          // Immer entry point has a if (process.env.NODE_ENV === 'production')
          // but |process| is not defined in the browser. Bypass.
          // https://github.com/immerjs/immer/issues/557
          {test: /process\.env\.NODE_ENV/g, replace: "'production'"},
        ],
      }),

      // Translate source maps to point back to the .ts sources.
      sourcemaps(),
    ].concat(maybeUglify()),
    onwarn: function (warning, warn) {
      if (warning.code === 'CIRCULAR_DEPENDENCY') {
        // Ignore circular dependency warnings coming from third party code.
        if (warning.message.includes('node_modules')) {
          return;
        }

        // Treat all other circular dependency warnings as errors.
        throw new Error(
          `Circular dependency detected in ${warning.importer}:\n\n  ${warning.cycle.join('\n  ')}`,
        );
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

function maybeUglify() {
  const minifyEnv = process.env['MINIFY_JS'];
  if (!minifyEnv) return [];
  const opts =
    minifyEnv === 'preserve_comments' ? {output: {comments: 'all'}} : undefined;
  return [uglify(opts)];
}

const maybeBigtrace = process.env['ENABLE_BIGTRACE']
  ? [defBundle('tsc/bigtrace', 'bigtrace', 'dist_version/bigtrace')]
  : [];

const maybeOpenPerfettoTrace = process.env['ENABLE_OPEN_PERFETTO_TRACE']
  ? [defBundle('tsc', 'open_perfetto_trace', 'dist/open_perfetto_trace')]
  : [];

module.exports = [
  defBundle('tsc', 'frontend', 'dist_version'),
  defBundle('tsc', 'engine', 'dist_version'),
  defBundle('tsc', 'traceconv', 'dist_version'),
  defBundle('tsc', 'chrome_extension', 'chrome_extension'),
  defServiceWorkerBundle(),
]
  .concat(maybeBigtrace)
  .concat(maybeOpenPerfettoTrace);
