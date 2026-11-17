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

// Where the UI is served from, sent with every event so that dashboards can
// leave out local builds and pull request previews.
export type PerfettoEnv = 'prod' | 'localhost' | 'pr_preview' | 'other';

export function getPerfettoEnv(url: URL): PerfettoEnv {
  if (url.hostname === 'ui.perfetto.dev') return 'prod';
  if (url.hostname === 'localhost' || url.hostname === '127.0.0.1') {
    return 'localhost';
  }
  // CI uploads pull request previews to
  // https://storage.googleapis.com/perfetto-ci-artifacts/gh-<run>-<attempt>-ui/.
  if (
    url.hostname === 'storage.googleapis.com' &&
    url.pathname.startsWith('/perfetto-ci-artifacts/')
  ) {
    return 'pr_preview';
  }
  // Anything else, so a new hosting location is never counted as prod.
  return 'other';
}
