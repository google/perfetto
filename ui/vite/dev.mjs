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

// Dev-mode patches to ui/src/assets/index.html: rewrite the inline bootstrap
// so it loads the TS entry through Vite's module graph, swap the version map
// for a single-channel dev map, and apply an optional title override. In prod
// the same shaping is done by cpHtml() in build.mjs.
export function pluginPatchIndexHtml({
  devVersion = '',
  devTitleOverride = '',
} = {}) {
  return {
    name: 'perfetto:patch-index-html',
    apply: 'serve',
    transformIndexHtml: {
      order: 'pre',
      handler(html) {
        let out = html.replace(
          /script\.src\s*=\s*version\s*\+\s*['"]\/frontend_bundle\.js['"];?/,
          `script.src = '/frontend/index.ts';`,
        );
        out = out.replace(
          /script\.async\s*=\s*true;?/,
          `script.type = 'module'; window.__GLOBAL_ASSET_ROOT__ = version + '/';`,
        );
        const versionMap = JSON.stringify({stable: devVersion || '.'});
        out = out.replace(
          /data-perfetto_version='[^']*'/,
          `data-perfetto_version='${versionMap}'`,
        );
        if (devTitleOverride) {
          out = out.replace(
            /<title>[^<]*<\/title>/,
            `<title>${devTitleOverride}</title>`,
          );
        }
        return out;
      },
    },
  };
}
