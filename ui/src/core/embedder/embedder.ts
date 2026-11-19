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

import type m from 'mithril';
import type {App} from '../../public/app';

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

  // The product this embedder represents (e.g. 'memscope'), or undefined for
  // the general purpose Perfetto UI. See products/README.md.
  readonly productId?: string;

  // Overrides the page title. Undefined keeps the title from index.html.
  readonly appTitle?: string;

  // Renders the home page (route '/'). Undefined uses Perfetto's home page.
  readonly renderHomePage?: () => m.Children;

  // If set, only these plugins (plus core plugins) are registered at all, so
  // the user cannot enable anything outside the product's scope.
  readonly pluginAllowlist?: ReadonlyArray<string>;

  // How the user moves between pages. Defaults to the sidebar.
  readonly navigation?: Navigation;
}

/** A top level page in the 'tabs' navigation style. */
export interface NavTab {
  readonly label: string;
  // The route page this tab shows, e.g. '/memory'. The tab is highlighted
  // while it is the current page.
  readonly page: string;
  // Where the tab goes. Defaults to `#!${page}`.
  readonly href?: () => string;
  // Whether to show the tab, e.g. only once a trace is loaded.
  readonly isVisible?: (app: App) => boolean;
}

/**
 * The navigation style of the UI:
 * - 'sidebar': Perfetto's sidebar, filled by plugins.
 * - 'tabs': no sidebar; a brand that links home and a fixed set of tabs in
 *   the topbar. Suits products with a handful of pages.
 */
export type Navigation =
  | {readonly kind: 'sidebar'}
  | {
      readonly kind: 'tabs';
      readonly brand: {readonly text: string; readonly icon?: string};
      readonly tabs: ReadonlyArray<NavTab>;
    };
