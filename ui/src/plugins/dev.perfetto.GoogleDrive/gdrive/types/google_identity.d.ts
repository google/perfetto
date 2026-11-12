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

// Minimal ambient types for Google Identity Services
// (https://accounts.google.com/gsi/client), which is loaded at runtime. Only
// the OAuth2 token client is declared, which this plugin uses to connect to the
// user's Google account.
//
// These are declared here rather than taken from @types/google.accounts to
// keep them to what the plugin calls.

declare global {
  // The GSI OAuth2 token client returned by google.accounts.oauth2
  // .initTokenClient().
  interface GDriveTokenClient {
    requestAccessToken(options?: {prompt?: string}): void;
  }

  interface GDriveTokenResponse {
    readonly error?: string;
    readonly access_token?: string;
    readonly expires_in?: string;
    readonly token_type?: string;
    readonly scope?: string;
  }

  interface GDriveTokenClientError {
    readonly type?: string;
  }

  // The GSI scripts attach everything to window.google. We type the oauth2
  // token client here via a Window augmentation.
  interface Window {
    google?: {
      accounts?: {
        oauth2?: {
          initTokenClient(params: {
            client_id: string;
            scope: string;
            callback?: (response: GDriveTokenResponse) => void;
            error_callback?: (error: GDriveTokenClientError) => void;
          }): GDriveTokenClient;
        };
      };
    };
  }
}

// We can only augment the global scope from an external module.
export {};
