// Copyright (C) 2020 The Android Open Source Project
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

// This script handles the caching of the UI resources, allowing it to work
// offline (as long as the UI site has been visited at least once).
// Design doc: http://go/perfetto-offline.

// When a new version of the UI is released (e.g. v1 -> v2), the following
// happens on the next visit:
// 1. The v1 (old) service worker is activated (at this point we don't know yet
//    that v2 is released).
// 2. /index.html is requested. The SW intercepts the request and serves
//    v1 from cache.
// 3. The browser checks if a new version of service_worker.js is available. It
//    does that by comparing the bytes of the current and new version.
// 5. service_worker.js v2 will not be byte identical with v1, even if v2 was a
//    css-only change. This is due to the hashes in UI_DIST_MAP below. For this
//    reason v2 is installed in the background (it takes several seconds).
// 6. The 'install' handler is triggered, the new resources are fetched and
//    populated in the cache.
// 7. The 'activate' handler is triggered. The old caches are deleted at this
//    point.
// 8. frontend/index.ts (in setupServiceWorker()) is notified about the activate
//    and shows a notification prompting to reload the UI.
//
// If the user just closes the tab or hits refresh, v2 will be served anyways
// on the next load.

// UI_DIST_FILES is map of {file_name -> sha1}.
// It is really important that this map is bundled directly in the
// service_worker.js bundle file, as it's used to cause the browser to
// re-install the service worker and re-fetch resources when anything changes.
// This is why the map contains the SHA1s even if we don't directly use them in
// the code (because it makes the final .js file content-dependent).

import {UI_DIST_MAP} from '../gen/dist_file_map';

declare var self: ServiceWorkerGlobalScope;

const LOG_TAG = `ServiceWorker[${UI_DIST_MAP.hex_digest.substr(0, 16)}]: `;

// TODO(primiano): Temporarily disabling service worker because our default
// cache policy (1d) made the response unreliable (b/148675312).

self.addEventListener('install', () => {
  self.skipWaiting();
});

self.console.debug(LOG_TAG + 'disabled due to b/148675312');