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

/**
 * Configuration for an embedder-provided default extension server.
 */
export interface EmbedderExtensionServer {
  readonly url: string;
  readonly authType: 'none' | 'https_sso';
}

/**
 * Interface for embedder-specific behavior. Different implementations allow
 * the UI to adapt to the environment it's running in (e.g. ui.perfetto.dev
 * vs a third-party embedding).
 */
export interface Embedder {
  // Returns the Google Analytics measurement ID, or undefined if analytics
  // should not be enabled for this embedder.
  readonly analyticsId: string | undefined;

  // Returns the default extension server that should be added on startup if
  // not already configured by the user. Undefined means no default.
  readonly extensionServer: EmbedderExtensionServer | undefined;
}
