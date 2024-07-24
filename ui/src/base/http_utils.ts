// Copyright (C) 2019 The Android Open Source Project
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

import {assertTrue} from './logging';

export function fetchWithTimeout(
  input: RequestInfo,
  init: RequestInit,
  timeoutMs: number,
) {
  return new Promise<Response>((resolve, reject) => {
    const timer = setTimeout(
      () =>
        reject(new Error(`fetch(${input}) timed out after ${timeoutMs} ms`)),
      timeoutMs,
    );
    fetch(input, init)
      .then((response) => resolve(response))
      .catch((err) => reject(err))
      .finally(() => clearTimeout(timer));
  });
}

/**
 * NOTE: this function can only be called from synchronous contexts. It will
 * fail if called in timer handlers or async continuations (e.g. after an await)
 * Use globals.root which caches it on startup.
 * @returns the directory where the app is served from, e.g. 'v46.0-a2082649b'
 */
export function getServingRoot() {
  // Works out the root directory where the content should be served from
  // e.g. `http://origin/v1.2.3/`.
  const script = document.currentScript as HTMLScriptElement;

  if (script === null) {
    // Can be null in tests.
    assertTrue(typeof jest !== 'undefined');
    return '';
  }

  let root = script.src;
  root = root.substring(0, root.lastIndexOf('/') + 1);
  return root;
}
