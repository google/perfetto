// Copyright (C) 2018 The Android Open Source Project
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

import {raf} from '../core/raf_scheduler';
import {AppImpl} from '../core/app_impl';
import {SqlPackage} from '../public/extra_sql_packages';

interface WindowWithGlobals {
  globals: {
    // This variable is set by the is_internal_user.js script if the user is a
    // googler. This is used to avoid exposing features that are not ready yet for
    // public consumption. The gated features themselves are not secret. If a user
    // has been detected as a Googler once, make that sticky in localStorage, so
    // that we keep treating them as such when they connect over public networks.
    // This is normally undefined is injected in via is_internal_user.js.
    // WARNING: do not change/rename/move without considering impact on the
    // internal_user script.
    isInternalUser: boolean;

    // WARNING: do not change/rename/move without considering impact on the
    // internal_user script.
    readonly extraSqlPackages: SqlPackage[];

    // Used when switching to the legacy TraceViewer UI.
    // Most resources are cleaned up by replacing the current |window| object,
    // however pending RAFs and workers seem to outlive the |window| and need to
    // be cleaned up explicitly.
    shutdown(): void;
  };
}

function initializeGlobals(app: AppImpl) {
  // Patch the global window object with a few hooks that point into the app
  // object.
  (window as unknown as WindowWithGlobals).globals = {
    get isInternalUser() {
      return app.isInternalUser;
    },
    set isInternalUser(value: boolean) {
      app.isInternalUser = value;
    },
    get extraSqlPackages(): SqlPackage[] {
      return app.extraSqlPackages;
    },
    shutdown() {
      raf.shutdown();
    },
  };
}

/**
 * This function is rather fragile on purpose and thwarted with legacy.
 *
 * It:
 * - Loads a script that detects if the user is a googler, and if so sets a flag
 *   in an object called `window.globals.isInternaluser`.
 * - It also loads some extra google internal SQL packages into
 *   window.globals.extraSqlPackages.
 * - Thus - window.globals is sort of an output of the script.
 * - However we don't want to actually store any state inside window.globals,
 *   everything should live inside app.
 * - So, we initially set up window.globals as a proxy for the app object.
 * - Then we load the script as a script tag, which the browser will run as soon
 *   as it's placed on the page.
 */
export async function loadIsInternalUserScript(app: AppImpl) {
  // First we need to set up the globals object and make it available to the script.
  initializeGlobals(app);

  await new Promise<void>((resolve) => {
    // If the script fails to load, we still want to initialize the analytics
    // system, so we set a timeout to do that.
    const script = document.createElement('script');
    script.src =
      'https://storage.cloud.google.com/perfetto-ui-internal/is_internal_user.js';
    script.async = true;
    script.onerror = () => resolve();
    script.onload = () => resolve();
    setTimeout(() => resolve(), 5000);

    document.head.append(script);
  });
}
