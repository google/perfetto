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

const esbuild = require('esbuild');
const path = require('path');

const ROOT_DIR = path.dirname(path.dirname(__dirname));
const OUT_SYMLINK = path.join(ROOT_DIR, 'ui/out');

const args = process.argv.slice(2);
const watch = args.includes('--watch');
const isMemory64Only = args.includes('--only-wasm-memory64');
const bigtrace = args.includes('--bigtrace');
const openPerfettoTrace = args.includes('--open-perfetto-trace');
const minifyArgIdx = args.indexOf('--minify-js');
const minifyJs = minifyArgIdx !== -1;

const traceProcessorReplacementPlugin = {
  name: 'trace-processor-replacement',
  setup(build) {
    build.onResolve({filter: /trace_processor_32_stub/}, (args) => {
      return {path: path.join(args.resolveDir, '../gen/trace_processor.js')};
    });
  },
};

const makeCtx = async (entryPoint, outDir, name) => {
  const entry = path.join(OUT_SYMLINK, 'tsc', entryPoint, 'index.js');
  const outfile = path.join(OUT_SYMLINK, outDir, name + '_bundle.js');

  const plugins = [];
  if (!isMemory64Only) {
    plugins.push(traceProcessorReplacementPlugin);
  }

  const ctx = await esbuild.context({
    entryPoints: [entry],
    outfile: outfile,
    bundle: true,
    platform: 'browser',
    format: 'iife',
    sourcemap: true,
    minify: minifyJs,
    external: ['ws'],
    plugins: plugins,
    define: {
      'process.env.NODE_ENV': '"production"',
      'process.env.IS_MEMORY64_ONLY': `${isMemory64Only}`,
    },
  });
  if (watch) {
    await ctx.watch();
  } else {
    await ctx.rebuild();
    await ctx.dispose();
  }
};

async function main() {
  const tasks = [
    makeCtx('frontend', 'dist_version', 'frontend'),
    makeCtx('engine', 'dist_version', 'engine'),
    makeCtx('traceconv', 'dist_version', 'traceconv'),
    makeCtx('chrome_extension', 'chrome_extension', 'chrome_extension'),
  ];
  if (bigtrace) {
    tasks.push(
        makeCtx('bigtrace/bigtrace', 'dist_version/bigtrace', 'bigtrace'),
    );
  }
  if (openPerfettoTrace) {
    tasks.push(
        makeCtx(
            'open_perfetto_trace',
            'dist/open_perfetto_trace',
            'open_perfetto_trace',
        ),
    );
  }
  await Promise.all(tasks);
}

main();
