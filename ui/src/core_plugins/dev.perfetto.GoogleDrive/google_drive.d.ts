// Copyright (C) 2025 The Android Open Source Project
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

// Ambient types for the parts of the Google Drive / Google Identity Services
// APIs that are not covered by the installed @types packages:
//   * gapi.client.drive.share (the Drive Share settings dialog)
//   * google.accounts.oauth2 (the GSI OAuth2 token client)
//
// We declare only the minimal surface this plugin actually uses.

declare global {
  // The Drive Share API is exposed under gapi.drive.share, NOT
  // gapi.client.drive.share (which is where the generated
  // @types/gapi.client.drive-v3 types live). Declare the minimal surface
  // this plugin uses.
  namespace gapi.drive {
    namespace share {
      class ShareClient {
        setOAuthToken(token: string): void;
        setItemIds(ids: string[]): void;
        showSettingsDialog(): void;
      }
    }
  }

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
