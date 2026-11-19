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

// Loads one or more modules through the Google API loader, which must already
// have been loaded (see ./types/gapi.d.ts). `modules` is colon separated, e.g.
// 'picker:drive-share'. Rejects if they fail to load, or have not loaded within
// `timeoutMs`.
//
// gapi.load() is callback based, and with a bare callback it reports neither
// failure nor a stall. Passing it an options object gets both.
export function loadGapiModules(
  modules: string,
  timeoutMs: number,
): Promise<void> {
  return new Promise<void>((resolve, reject) => {
    gapi.load(modules, {
      callback: () => resolve(),
      onerror: () => reject(new Error(`Failed to load gapi: ${modules}`)),
      timeout: timeoutMs,
      ontimeout: () => reject(new Error(`Timed out loading gapi: ${modules}`)),
    });
  });
}
