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

import {AppImpl} from '../core/app_impl';
import {raf} from '../core/raf_scheduler';
import {SqlPackage} from '../public/extra_sql_packages';

const HARDCODED_DESCRIPTOR_BASE64 = 'CvADCmBsb2dzL3Byb3RvL3dpcmVsZXNzL2FuZHJvaWQvc3RhdHMvcGxhdGZvcm0vd2VzdHdvcmxkL2F0b21zL2JhdHRlcnkvYmF0dGVyeV9leHRlbnNpb25fYXRvbXMucHJvdG8SPGxvZ3MucHJvdG8ud2lyZWxlc3MuYW5kcm9pZC5zdGF0cy5wbGF0Zm9ybS53ZXN0d29ybGQuYmF0dGVyeSJxChxSYXdCYXR0ZXJ5R2F1Z2VTdGF0c1JlcG9ydGVkEh8KF3N5c3RlbV9jbG9ja190aW1lX25hbm9zGAEgAygDEhUKDXZvbHRhZ2Vfdm9sdHMYAiADKAISGQoRY3VycmVudF9taWxsaWFtcHMYAyADKAI6ngEKIHJhd19iYXR0ZXJ5X2dhdWdlX3N0YXRzX3JlcG9ydGVkEhcuYW5kcm9pZC5vcy5zdGF0c2QuQXRvbRi9CCABKAsyWi5sb2dzLnByb3RvLndpcmVsZXNzLmFuZHJvaWQuc3RhdHMucGxhdGZvcm0ud2VzdHdvcmxkLmJhdHRlcnkuUmF3QmF0dGVyeUdhdWdlU3RhdHNSZXBvcnRlZEItChZjb20uYW5kcm9pZC5vcy5iYXR0ZXJ5UAGSAwQQAiAD0u+AkAIGbGF0ZXN0YghlZGl0aW9uc3DoBwpVCiZzeW50aGV0aWMvYW5kcm9pZC9vcy9zdGF0c2QvYXRvbS5wcm90bxIRYW5kcm9pZC5vcy5zdGF0c2QiEAoEQXRvbSoICAEQgICAgAJiBnByb3RvMg==';

// This controls how long we wait for the script to load before giving up and
// proceeding as if the user is not internal.
const SCRIPT_LOAD_TIMEOUT_MS = 5000;
const SCRIPT_URL =
  'https://storage.cloud.google.com/perfetto-ui-internal/is_internal_user.js';

// This interface describes the required interface that the script expect to
// find on window.globals.
interface Globals {
  // This variable is set by the is_internal_user.js script if the user is a
  // googler. This is used to avoid exposing features that are not ready yet for
  // public consumption. The gated features themselves are not secret. If a user
  // has been detected as a Googler once, make that sticky in localStorage, so
  // that we keep treating them as such when they connect over public networks.
  // This is normally undefined is injected in via is_internal_user.js.
  // WARNING: do not change/rename/move without considering impact on the
  // internal_user script.
  isInternalUser: boolean;

  // The script adds to this list, hence why it's readonly.
  // WARNING: do not change/rename/move without considering impact on the
  // internal_user script.
  readonly extraSqlPackages: SqlPackage[];

  extraParsingDescriptorsBase64: string;

  // TODO(stevegolton): Check if we actually need to use these.
  // Used when switching to the legacy TraceViewer UI.
  // Most resources are cleaned up by replacing the current |window| object,
  // however pending RAFs and workers seem to outlive the |window| and need to
  // be cleaned up explicitly.
  shutdown(): void;
}

/**
 * Sets up a proxy object on window.globals that forwards property accesses to
 * the given AppImpl instance.
 */
function setupGlobalsProxy(app: AppImpl) {
  // Patch the global window object with a few hooks that point into the app
  // object.
  (window as unknown as {globals?: Globals}).globals = {
    get isInternalUser() {
      return app.isInternalUser;
    },
    set isInternalUser(value: boolean) {
      app.isInternalUser = value;
    },
    get extraSqlPackages(): SqlPackage[] {
      return app.extraSqlPackages;
    },
    get extraParsingDescriptorsBase64(): string {
      return app.extraParsingDescriptorsBase64;
    },
    set extraParsingDescriptorsBase64(value: string) {
      app.extraParsingDescriptorsBase64 = value;
    },
    shutdown() {
      raf.shutdown();
    },
  };
}

/**
 * Loads a script that detects if the user is internal, allowing access to
 * non-public features and SQL packages.
 *
 * This function works by creating a temporary `window.globals` object that
 * acts as a proxy to the main `AppImpl` instance. An external script is then
 * loaded, which populates properties on `window.globals`. These properties are
 * transparently forwarded to the `AppImpl` instance.
 */
export async function tryLoadIsInternalUserScript(app: AppImpl): Promise<void> {
  // Set up the global object and attach it to `window` before loading the
  // script.
  setupGlobalsProxy(app);

  console.log('PROTOTYPE: Using hardcoded descriptors for testing.');

  app.extraParsingDescriptorsBase64 = `if (globals.extraParsingDescriptorsBase64) { globals.extraParsingDescriptorsBase64 = '${HARDCODED_DESCRIPTOR_BASE64}'; }`;


  await new Promise<void>((resolve) => {
    const script = document.createElement('script');
    script.src = SCRIPT_URL;
    script.async = true;
    script.onerror = () => resolve();
    script.onload = () => resolve();
    document.head.append(script);

    // Set a timeout to avoid blocking the UI for too long if the script is slow
    // to load.
    setTimeout(() => resolve(), SCRIPT_LOAD_TIMEOUT_MS);
  });
}
