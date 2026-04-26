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

// Copies the non-JS static assets into the dist tree. The rules here mirror
// the RULES array in the old ui/build.js.

import fs from 'node:fs';
import {dirname, join, relative} from 'node:path';
import {ROOT_DIR, UI_DIR, cp, ensureDir, walk} from './common.mjs';

const ASSET_RULES = [
  // [ sourceRootRelative, regex, destSubdir ]
  // The regex is matched against the path relative to ROOT_DIR. The first
  // capture group (if any) determines the relative path *inside* destSubdir.
  // If no capture group is used, the file's basename is used.
  {
    src: 'ui/src/assets',
    re: /^ui\/src\/assets\/((.*)\.png)$/,
    dest: 'assets',
  },
  {
    src: 'ui/src/assets/data_explorer',
    re: /^ui\/src\/assets\/(data_explorer\/base-page\.json)$/,
    dest: 'assets',
  },
  {
    src: 'ui/src/assets/data_explorer/examples',
    re: /^ui\/src\/assets\/(data_explorer\/examples\/.*\.json)$/,
    dest: 'assets',
  },
  {
    src: 'ui/src/assets/data_explorer/node_info',
    re: /^ui\/src\/assets\/(data_explorer\/node_info\/.*\.md)$/,
    dest: 'assets',
  },
  {
    src: 'buildtools/typefaces',
    re: /^buildtools\/typefaces\/(.+\.woff2)$/,
    dest: 'assets',
  },
  {
    src: 'buildtools/catapult_trace_viewer',
    re: /^buildtools\/catapult_trace_viewer\/(.+\.(?:js|html))$/,
    dest: 'assets',
  },
];

export function copyStaticAssets({outDistDir, outBigtraceDistDir}) {
  const seen = new Set();
  for (const rule of ASSET_RULES) {
    const srcDir = join(ROOT_DIR, rule.src);
    if (!fs.existsSync(srcDir)) continue;
    walk(srcDir, (absPath) => {
      const rel = relative(ROOT_DIR, absPath);
      const m = rule.re.exec(rel);
      if (!m) return;
      const keyedName = m[1] ?? absPath;
      if (seen.has(keyedName)) return;
      seen.add(keyedName);
      const dst = join(outDistDir, rule.dest, keyedName);
      cp(absPath, dst);
      if (outBigtraceDistDir) {
        const dstBig = join(outBigtraceDistDir, rule.dest, keyedName);
        cp(absPath, dstBig);
      }
    });
  }
}

export function copyChromeExtensionAssets({outExtDir}) {
  ensureDir(outExtDir);
  cp(
    join(UI_DIR, 'src/assets/logo-128.png'),
    join(outExtDir, 'logo-128.png'),
  );
  cp(
    join(UI_DIR, 'src/chrome_extension/manifest.json'),
    join(outExtDir, 'manifest.json'),
  );
}

// Copies bigtrace.html / index.html / open_perfetto_trace/index.html into
// the versioned dist dir. The duplication into dist/index.html (with version
// map patching) happens in postbuild.mjs.
export function copyHtml({
  outDistDir,
  outOpenPerfettoTraceDistDir,
  bigtrace = false,
  openPerfettoTrace = false,
}) {
  cp(
    join(UI_DIR, 'src/assets/index.html'),
    join(outDistDir, 'index.html'),
  );
  if (bigtrace) {
    cp(
      join(UI_DIR, 'src/assets/bigtrace.html'),
      join(outDistDir, 'bigtrace.html'),
    );
  }
  if (openPerfettoTrace && outOpenPerfettoTraceDistDir) {
    cp(
      join(UI_DIR, 'src/open_perfetto_trace/index.html'),
      join(outOpenPerfettoTraceDistDir, 'index.html'),
    );
  }
}
