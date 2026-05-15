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

import {assertTrue} from './assert';

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
  // import.meta.url is the URL of *this* module file after bundling, which
  // sits next to frontend_bundle.js in the same /v1.2.3/ directory. Strip the
  // filename to get the serving root.
  const url = import.meta.url;
  if (!url || url.startsWith('file://')) {
    // Jest / node test contexts: no meaningful serving root.
    assertTrue(typeof jest !== 'undefined');
    return '';
  }
  return url.substring(0, url.lastIndexOf('/') + 1);
}
