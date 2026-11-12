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

// Minimal ambient types for gapi, the global added by the Google API loader
// (https://apis.google.com/js/api.js), which is loaded at runtime. Only the
// parts this plugin uses are declared: the module loader, and the Drive
// sharing dialog from the 'drive-share' module.
//
// These are declared here rather than taken from @types/gapi to keep them to
// what the plugin calls.

declare global {
  namespace gapi {
    function load(
      apiName: string,
      callbackOrConfig:
        | (() => void)
        | {
            callback: () => void;
            onerror?: () => void;
            timeout?: number;
            ontimeout?: () => void;
          },
    ): void;

    namespace drive {
      namespace share {
        class ShareClient {
          setOAuthToken(token: string): void;
          setItemIds(ids: string[]): void;
          showSettingsDialog(): void;
        }
      }
    }
  }
}

// We can only augment the global scope from an external module.
export {};
