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

import path from 'node:path';
import fs from 'node:fs';

// Re-anchors relative imports made from generated files under |genSymlink|
// (e.g. import barrels). Without this, Rollup canonicalises the symlink to
// out/.../tsc/gen and the relative imports point at a directory that doesn't
// contain the source files.
export function pluginGenRelativeImports({genSymlink}) {
  const genReal = fs.existsSync(genSymlink) ? fs.realpathSync(genSymlink) : '';
  return {
    name: 'perfetto:gen-relative-imports',
    enforce: 'pre',
    async resolveId(source, importer) {
      if (!importer || !source.startsWith('.')) return null;
      if (!genReal || !importer.startsWith(genReal + path.sep)) return null;
      const reanchored = path.resolve(genSymlink, source);
      return this.resolve(reanchored, importer, {skipSelf: true});
    },
  };
}
