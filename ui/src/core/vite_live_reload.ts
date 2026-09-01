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

import {debounce} from '../base/rate_limiters';

// Thrown to cancel Vite's automatic page reload in favour of our debounced
// confirm prompt. error_dialog.ts filters this message out.
export const VITE_RELOAD_CANCEL_MSG = 'Cancel vite reload';
const DEBOUNCE_DELAY_MS = 250;

// When served by the Vite dev server, subscribe to its full-reload events and
// confirm with the user before reloading. In a prod build `import.meta.hot` is
// undefined and this is a no-op.
export function initViteLiveReload() {
  if (!import.meta.hot) return;
  const pending = new Set<string>();
  const prompt = debounce(() => {
    const paths = [...pending];
    pending.clear();
    const list =
      paths.length <= 3
        ? paths.map((p) => `  • ${p}`).join('\n')
        : paths
            .slice(0, 3)
            .map((p) => `  • ${p}`)
            .join('\n') + `\n  • …and ${paths.length - 3} more`;
    const msg = `Modules changed on disk:\n${list}\n\nReload now?`;
    if (confirm(msg)) location.reload();
  }, DEBOUNCE_DELAY_MS);
  import.meta.hot.on('vite:beforeFullReload', (payload) => {
    const p = payload as {path?: string; triggeredBy?: string};
    // `path` may be "*" or missing; `triggeredBy` is the absolute path of the
    // file that changed. Prefer the latter, shortened to a repo-relative form.
    const raw = p.triggeredBy ?? p.path ?? 'unknown';
    const shown = raw.includes('/') ? raw.split('/').slice(-2).join('/') : raw;
    pending.add(shown);
    prompt();
    // Block Vite's automatic reload; prompt() will trigger one on accept.
    // Throwing an error is the only way to reliably cancel the reload, since
    // Vite doesn't provide an API for that.
    throw new Error(VITE_RELOAD_CANCEL_MSG);
  });
}
