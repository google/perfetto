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

// Post-build steps that the Vite builds don't handle on their own:
//   - generateServiceWorkerManifest: walks dist/<ver>/ and writes
//     dist/<ver>/manifest.json with sha256 hashes for every asset. The
//     ServiceWorker fetches this and precaches the entries (see
//     ui/src/service_worker/service_worker.ts::installAppVersionIntoCache).
//   - writeRootIndexHtml: duplicates dist/<ver>/index.html into dist/index.html
//     with the `data-perfetto_version` attribute patched to a JSON channel
//     map (so the runtime bootstrap picks the right versioned subdir).

import crypto from 'node:crypto';
import fs from 'node:fs';
import {join, relative} from 'node:path';
import {walk} from './common.mjs';

// Files that must NOT appear in manifest.json:
//   - manifest.json itself
//   - index.html (SW fetches this separately from network with timeout)
//   - *.map (do not cache source maps offline)
const MANIFEST_SKIP = /(\.map|manifest\.json|index\.html)$/;

export function generateServiceWorkerManifest(outDistDir) {
  const manifest = {resources: {}};
  walk(
    outDistDir,
    (absPath) => {
      const rel = relative(outDistDir, absPath);
      const contents = fs.readFileSync(absPath);
      const b64 = crypto.createHash('sha256').update(contents).digest('base64');
      manifest.resources[rel] = 'sha256-' + b64;
    },
    {skipRegex: MANIFEST_SKIP},
  );
  fs.writeFileSync(
    join(outDistDir, 'manifest.json'),
    JSON.stringify(manifest, null, 2),
  );
}

// Reads `dist/<ver>/<indexFile>`, patches data-perfetto_version to a
// {"stable":"<ver>"} map, optionally overrides the <title>, and writes the
// result to `dist/<indexFile>`. The unmodified copy at the versioned path
// stays in place so `ui.perfetto.dev/<ver>/` continues to serve an
// archival build.
export function writeRootIndexHtml({
  outDistDir,
  outDistRootDir,
  version,
  titleOverride = '',
  indexFile = 'index.html',
}) {
  const versionedPath = join(outDistDir, indexFile);
  if (!fs.existsSync(versionedPath)) return;
  let html = fs.readFileSync(versionedPath, 'utf8');
  const versionMap = JSON.stringify({stable: version});
  html = html.replace(
    /data-perfetto_version='[^']*'/,
    `data-perfetto_version='${versionMap}'`,
  );
  if (titleOverride) {
    html = html.replace(/<title>[^<]*<\/title>/, `<title>${titleOverride}</title>`);
  }
  fs.writeFileSync(join(outDistRootDir, indexFile), html);
}
