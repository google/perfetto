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

// Handles registration, unregistration and lifecycle of the service worker.
// This class contains only the controlling logic, all the code in here runs in
// the main thread, not in the service worker thread.
// The actual service worker code is in src/service_worker.
// Design doc: http://go/perfetto-offline.

import {reportError} from '../base/logging';
import {raf} from '../core/raf_scheduler';

// We use a dedicated |caches| object to share a global boolean beween the main
// thread and the SW. SW cannot use local-storage or anything else other than
// IndexedDB (which would be overkill).
const BYPASS_ID = 'BYPASS_SERVICE_WORKER';

class BypassCache {
  static async isBypassed(): Promise<boolean> {
    try {
      return await caches.has(BYPASS_ID);
    } catch (_) {
      // TODO(288483453): Reinstate:
      // return ignoreCacheUnactionableErrors(e, false);
      return false;
    }
  }

  static async setBypass(bypass: boolean): Promise<void> {
    try {
      if (bypass) {
        await caches.open(BYPASS_ID);
      } else {
        await caches.delete(BYPASS_ID);
      }
    } catch (_) {
      // TODO(288483453): Reinstate:
      // ignoreCacheUnactionableErrors(e, undefined);
    }
  }
}

export class ServiceWorkerController {
  private _bypassed = false;
  private _installing = false;

  constructor(private servingRoot: string) {}

  // Caller should reload().
  async setBypass(bypass: boolean) {
    if (!('serviceWorker' in navigator)) return; // Not supported.
    this._bypassed = bypass;
    if (bypass) {
      await BypassCache.setBypass(true); // Create the entry.
      for (const reg of await navigator.serviceWorker.getRegistrations()) {
        await reg.unregister();
      }
    } else {
      await BypassCache.setBypass(false);
      // eslint-disable-next-line @typescript-eslint/strict-boolean-expressions
      if (window.localStorage) {
        window.localStorage.setItem('bypassDisabled', '1');
      }
      this.install();
    }
    raf.scheduleFullRedraw();
  }

  onStateChange(sw: ServiceWorker) {
    raf.scheduleFullRedraw();
    if (sw.state === 'installing') {
      this._installing = true;
    } else if (sw.state === 'activated') {
      this._installing = false;
    }
  }

  monitorWorker(sw: ServiceWorker | null) {
    if (!sw) return;
    sw.addEventListener('error', (e) => reportError(e));
    sw.addEventListener('statechange', () => this.onStateChange(sw));
    this.onStateChange(sw); // Trigger updates for the current state.
  }

  async install() {
    const versionDir = this.servingRoot.split('/').slice(-2)[0];

    if (!('serviceWorker' in navigator)) return; // Not supported.

    if (location.pathname !== '/') {
      // Disable the service worker when the UI is loaded from a non-root URL
      // (e.g. from the CI artifacts GCS bucket). Supporting the case of a
      // nested index.html is too cumbersome and has no benefits.
      return;
    }

    // If this is localhost disable the service worker by default, unless the
    // user manually re-enabled it (in which case bypassDisabled = '1').
    const hostname = location.hostname;
    const isLocalhost = ['127.0.0.1', '::1', 'localhost'].includes(hostname);
    const bypassDisabled =
      // eslint-disable-next-line @typescript-eslint/strict-boolean-expressions
      window.localStorage &&
      window.localStorage.getItem('bypassDisabled') === '1';
    if (isLocalhost && !bypassDisabled) {
      await this.setBypass(true); // Will cause the check below to bail out.
    }

    if (await BypassCache.isBypassed()) {
      this._bypassed = true;
      console.log('Skipping service worker registration, disabled by the user');
      return;
    }
    // In production cases versionDir == VERSION. We use this here for ease of
    // testing (so we can have /v1.0.0a/ /v1.0.0b/ even if they have the same
    // version code).
    const swUri = `/service_worker.js?v=${versionDir}`;
    navigator.serviceWorker.register(swUri).then((registration) => {
      // At this point there are two options:
      // 1. This is the first time we visit the site (or cache was cleared) and
      //    no SW is installed yet. In this case |installing| will be set.
      // 2. A SW is already installed (though it might be obsolete). In this
      //    case |active| will be set.
      this.monitorWorker(registration.installing);
      this.monitorWorker(registration.active);

      // Setup the event that shows the "Updated to v1.2.3" notification.
      registration.addEventListener('updatefound', () => {
        this.monitorWorker(registration.installing);
      });
    });
  }

  get bypassed() {
    return this._bypassed;
  }
  get installing() {
    return this._installing;
  }
}
