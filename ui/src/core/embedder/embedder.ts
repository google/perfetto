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

import m from 'mithril';
import {App} from '../../public/app';

/**
 * Attrs passed to an embedder-supplied home-page component. Carrying the
 * `App` instance via Mithril attrs lets embedders use commands/settings
 * without importing `AppImpl`, which would form a circular import (the
 * embedder is referenced from `AppImpl` via `createEmbedder`).
 */
export interface HomePageAttrs {
  readonly app: App;
}

/**
 * Configuration for an embedder-provided default extension server.
 */
export interface EmbedderExtensionServer {
  readonly url: string;
  readonly authType: 'none' | 'https_sso';
}

/**
 * Configuration for a branding badge displayed in the sidebar header.
 *
 * Example: To add a text-only branding badge, set in your embedder:
 *   readonly brandingBadge = {text: 'BRAND', color: '#e07020'};
 */
export interface BrandingBadge {
  /** The text to display, e.g. "BRAND". */
  readonly text: string;
  /** CSS color for the text, e.g. "#e07020". */
  readonly color?: string;
  /** Optional Material icon name to display before the text. */
  readonly icon?: string;
  /**
   * Optional inline image data URI to display before the text (e.g.
   * `data:image/svg+xml;base64,...`). Takes precedence over `icon` when
   * both are specified. Only data URIs are supported; external URLs are not
   * allowed.
   */
  readonly image?: string;
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

  // Returns the branding badge to display in the sidebar header, or undefined
  // if no custom branding should be shown.
  readonly brandingBadge: BrandingBadge | undefined;

  // Returns the list of plugin IDs that should be enabled by default.
  readonly defaultPlugins: ReadonlyArray<string>;

  // Mithril component rendered at route '/'. Receives the running App via
  // attrs so embedder-supplied components can reach commands/settings/the
  // router without importing AppImpl. Undefined falls back to the
  // built-in HomePage.
  readonly homePage: m.ComponentTypes<HomePageAttrs> | undefined;

  // Sidebar wordmark image. Undefined falls back to the bundled Perfetto
  // wordmark; embedders override to swap it out.
  readonly brandLogo: {readonly src: string; readonly alt?: string} | undefined;
}
