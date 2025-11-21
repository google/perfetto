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
const json = require('@rollup/plugin-json');
const {SourceMapConsumer, SourceMapGenerator} = require('source-map');

const ROOT_DIR = path.dirname(path.dirname(__dirname)); // The repo root.
const OUT_SYMLINK = path.join(ROOT_DIR, 'ui/out');

// Plugin to embed minimal source maps directly into bundles
function embedMinimalSourceMap() {
  return {
    name: 'embed-minimal-sourcemap',
    async generateBundle(options, bundle) {
      for (const chunk of Object.values(bundle)) {
        if (!chunk.fileName || !chunk.fileName.endsWith('_bundle.js') || !chunk.map) {
          continue;
        }

        try {
          // Create minimal source map from Rollup's full map
          const consumer = await new SourceMapConsumer(chunk.map);
          const generator = new SourceMapGenerator({
            file: chunk.map.file,
          });

          // Track which lines we've seen to only add one mapping per line
          const seenLines = new Set();
          
          // Clean source paths
          const cleanSourcePath = (source) => {
            let cleaned = source.replace('../../../out/ui/', '');
            cleaned = cleaned.replace('../../node_modules/', 'node_modules/');
            return cleaned;
          };

          consumer.eachMapping((mapping) => {
            if (!mapping.source) return;
            
            // Only add first mapping per generated line
            const lineKey = mapping.generatedLine;
            if (seenLines.has(lineKey)) return;
            seenLines.add(lineKey);

            const cleanSource = cleanSourcePath(mapping.source);
            
            generator.addMapping({
              generated: {
                line: mapping.generatedLine,
                column: 0, // First column only
              },
              original: {
                line: mapping.originalLine,
                column: mapping.originalColumn,
              },
              source: cleanSource,
              // name intentionally omitted to strip name references
            });
          });

          consumer.destroy();

          const minimalMap = JSON.parse(generator.toString());
          
          // Remove sourcesContent to reduce size
          delete minimalMap.sourcesContent;
          // Remove names array (should be empty anyway since we didn't add names)
          delete minimalMap.names;

          // Embed the minimal map at the end of the bundle using a registry
          // Use 'self' instead of 'window' for worker compatibility
          // Each bundle registers its map with its filename as the key
          chunk.code += `\n;(self.__SOURCEMAPS=self.__SOURCEMAPS||{})['${chunk.fileName}']=${JSON.stringify(minimalMap)};`;

          if (process.env.VERBOSE) {
            console.log(`Embedded minimal source map into ${chunk.fileName}`);
          }
        } catch (err) {
          console.error(`Error creating minimal source map for ${chunk.fileName}:`, err.message);
          // Don't fail the build, just skip embedding
        }
      }
    },
  };
}

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
    watch: {
      exclude: ['out/**'],
      buildDelay: 250,
    },
    plugins: [
      replace({
        patterns:
          process.env['IS_MEMORY64_ONLY'] != 'true'
            ? [
                {
                  test: './trace_processor_32_stub',
                  replace: '../gen/trace_processor',
                },
              ]
            : [],
      }),

      nodeResolve({
        mainFields: ['browser'],
        browser: true,
        preferBuiltins: false,
      }),

      commonjs({
        strictRequires: true,
      }),


      json(),

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
      
      // Embed minimal source map for error reporting
      embedMinimalSourceMap(),
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
