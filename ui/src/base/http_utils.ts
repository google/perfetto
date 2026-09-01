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

export function fetchWithProgress(
  url: string,
  onProgress?: (percentage: number) => void,
): Promise<Blob> {
  return new Promise((resolve, reject) => {
    const xhr = new XMLHttpRequest();

    xhr.open('GET', url, /* async= */ true);
    xhr.responseType = 'blob';

    xhr.onprogress = (event) => {
      if (event.lengthComputable) {
        const percentComplete = Math.round((event.loaded / event.total) * 100);
        onProgress?.(percentComplete);
      }
    };

    xhr.onload = () => {
      if (xhr.status >= 200 && xhr.status < 300) {
        resolve(xhr.response); // Resolve with the Blob response
      } else {
        reject(
          new Error(`Failed to download: ${xhr.status} ${xhr.statusText}`),
        );
      }
    };

    xhr.onerror = () => {
      reject(new Error(`Network error in fetchWithProgress(${url})`));
    };

    xhr.send();
  });
}

/**
 * @returns the directory the app is served from, e.g.
 *   'https://ui.perfetto.dev/v46.0-a2082649b/'.
 */
export function getServingRoot() {
  // In dev mode, window.__GLOBAL_ASSET_ROOT__ is set to the root URL of the dev
  // server's asset directory the bootstrap code in index.html before the
  // frontend bundle is loaded. In prod, this will be undefined, so we fall back
  // to the document.currentScript logic below.
  const window = globalThis as unknown as {__GLOBAL_ASSET_ROOT__?: string};
  if (window.__GLOBAL_ASSET_ROOT__) {
    return window.__GLOBAL_ASSET_ROOT__;
  }

  // Works out the root directory where the content should be served from
  // e.g. `http://origin/v1.2.3/`.
  const script = document.currentScript as HTMLScriptElement | null;
  if (!script) return '/';
  let root = script.src;
  root = root.substring(0, root.lastIndexOf('/') + 1);
  return root;
}
