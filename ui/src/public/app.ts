// Copyright (C) 2024 The Android Open Source Project
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

import {RouteArgs} from './route_schema';
import {CommandManager} from './command';
import {OmniboxManager} from './omnibox';
import {SidebarManager} from './sidebar';
import {Analytics} from './analytics';
import {PluginManager} from './plugin';
import {Trace} from './trace';
import {PageManager} from './page';
import {FeatureFlagManager} from './feature_flag';
import {Raf} from './raf';
import {SettingsManager} from './settings';
import {TraceStream} from './stream';

/**
 * The API endpoint to interact programmatically with the UI before a trace has
 * been loaded. This is passed to plugins' `onActivate()` lifecycle method and
 * provides access to app-wide functionality.
 */
export interface App {
  /**
   * Manages command registration and execution.
   *
   * Commands are user-invokable actions that can be triggered via hotkeys,
   * sidebar menu items, or programmatically. Use this to register plugin
   * commands that users can execute.
   */
  readonly commands: CommandManager;

  /**
   * Manages sidebar menu items across different sections.
   *
   * The sidebar is organized into sections (navigation, settings, support,
   * etc.). Use this to add menu entries that can navigate to pages, execute
   * actions, or trigger commands. Menu items are automatically removed when
   * the trace is closed or the plugin is unloaded.
   */
  readonly sidebar: SidebarManager;

  /**
   * Provides non-modal user prompts integrated into the UI.
   *
   * The omnibox can prompt users for free-form text input or selection from a
   * list of choices. Think of it as window.prompt() but non-blocking and
   * better integrated with the UI. Returns a promise that resolves to the
   * user's input or undefined if dismissed.
   */
  readonly omnibox: OmniboxManager;

  /**
   * Logs analytics events and errors.
   *
   * Use this to track user actions, trace operations, and error conditions.
   * Events are categorized (e.g., 'Trace Actions', 'User Actions') and can
   * be used to understand how users interact with your plugin.
   */
  readonly analytics: Analytics;

  /**
   * Provides access to registered plugins.
   *
   * Use this to get instances of other plugins (for inter-plugin
   * communication) or access metric visualizations. Note that plugin instances
   * are only available after a trace has been loaded.
   */
  readonly plugins: PluginManager;

  /**
   * Manages custom page registration and routing.
   *
   * Use this to register pages that respond to specific routes (e.g.,
   * '/settings', '/query'). Pages are automatically unregistered when the
   * trace is closed or the plugin is unloaded.
   */
  readonly pages: PageManager;

  /**
   * Manages feature flags for experimental or togglable features.
   *
   * Feature flags allow plugins to expose experimental functionality that
   * users can enable/disable. Flags are persisted across sessions and can be
   * configured on the flags page.
   */
  readonly featureFlags: FeatureFlagManager;

  /**
   * Manages persistent user-configurable settings.
   *
   * Settings are stored in local storage and can be configured on the settings
   * page. They support validation via Zod schemas, custom rendering, and can
   * optionally require an app reload when changed.
   */
  readonly settings: SettingsManager;

  /**
   * The initial URL query parameters when the app was first loaded.
   *
   * Contains parsed route arguments (query string parameters) from the initial
   * page load, before any client-side navigation occurred. Useful for
   * implementing deep linking and processing startup commands.
   */
  readonly initialRouteArgs: RouteArgs;

  /**
   * The currently loaded trace, if any.
   *
   * This is undefined until a trace has been loaded. The returned instance
   * is scoped to the calling plugin and will automatically clean up
   * plugin-specific resources when the trace is closed.
   */
  readonly trace?: Trace;

  /**
   * Request Animation Frame (RAF) scheduler for UI updates.
   *
   * Use this to schedule DOM and canvas redraws efficiently, or register
   * callbacks that run on canvas redraw cycles. Essential for implementing
   * custom visualizations or animations.
   */
  readonly raf: Raf;

  /**
   * Whether the current user is an internal user (e.g., Googler).
   *
   * Plugins may use this to conditionally show internal-only features,
   * experimental functionality, or internal documentation links. This is
   * typically true for users accessing ui.perfetto.dev from within Google's
   * network.
   */
  readonly isInternalUser: boolean;

  /**
   * Navigates to a new page by setting the URL hash.
   *
   * @param newHash - The new hash to navigate to (e.g., '#!/viewer',
   *   '#!/record'). The hash should include the '#!' prefix.
   */
  navigate(newHash: string): void;

  /**
   * Opens a trace from a file handle.
   *
   * This is typically used when the user selects a trace file through a file
   * picker dialog. The file is loaded into the trace processor and a new Trace
   * instance is returned.
   *
   * @param file - The File object to load as a trace.
   * @returns A promise that resolves to the loaded Trace instance.
   */
  openTraceFromFile(file: File): Promise<Trace>;

  /**
   * Opens a trace from a URL.
   *
   * The URL must be CORS-enabled and included in the Content Security Policy
   * (CSP) allowlist. This is commonly used to load traces from remote servers
   * or cloud storage.
   *
   * @param url - The URL of the trace file to load.
   * @returns A promise that resolves to the loaded Trace instance.
   */
  openTraceFromUrl(url: string): Promise<Trace>;

  /**
   * Opens a trace directly from a stream.
   *
   * This allows for custom trace loading scenarios where the trace data is
   * provided through a stream interface rather than a file or URL.
   *
   * @param stream - A TraceStream instance providing the trace data.
   * @returns A promise that resolves to the loaded Trace instance.
   */
  openTraceFromStream(stream: TraceStream): Promise<Trace>;

  /**
   * Opens a trace from an in-memory buffer.
   *
   * This is useful when you have trace data already loaded in memory (e.g.,
   * from a custom data source, generated programmatically, or received via
   * WebSocket).
   *
   * @param args - Configuration for loading the trace.
   * @param args.buffer - The ArrayBuffer containing the trace data.
   * @param args.title - A human-readable title for this trace.
   * @param args.fileName - The filename to associate with this trace (used in
   *   the UI and for downloads).
   * @returns A promise that resolves to the loaded Trace instance.
   */
  openTraceFromBuffer(args: {
    buffer: ArrayBuffer;
    title: string;
    fileName: string;
  }): Promise<Trace>;

  /**
   * Closes the currently loaded trace.
   *
   * This unloads the trace from the trace processor, cleans up all associated
   * resources (including plugin instances, tracks, and event listeners), and
   * returns the UI to its initial state. If no trace is currently loaded, this
   * method has no effect.
   */
  closeCurrentTrace(): void;
}
