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

import type {Analytics} from './analytics';
import type {CommandManager} from './commands';
import type {FeatureFlagManager} from './feature_flag';
import type {OmniboxManager} from './omnibox';
import type {PageManager} from './page';
import type {PluginManager} from './plugin';
import type {Raf} from './raf';
import type {RouteArgs} from './route_schema';
import type {SettingsManager} from './settings';
import type {SidePanelManager} from './side_panel';
import type {SidebarManager} from './sidebar';
import type {TraceStream} from './stream';
import type {TaskTracker} from './task_tracker';
import type {Trace} from './trace';

// A broken down representation of a route.
// For instance: #!/record/gpu?local_cache_key=a0b1
// becomes: {page: '/record', subpage: '/gpu', args: {local_cache_key: 'a0b1'}}
export interface Route {
  page: string;
  subpage: string;
  fragment: string;
  args: RouteArgs;
}

/**
 * The API endpoint to interact programmatically with the UI before a trace has
 * been loaded. This is passed to plugins' OnActivate().
 */
export interface App {
  readonly commands: CommandManager;
  readonly sidebar: SidebarManager;
  readonly omnibox: OmniboxManager;
  readonly analytics: Analytics;
  readonly plugins: PluginManager;
  readonly pages: PageManager;
  readonly featureFlags: FeatureFlagManager;
  readonly settings: SettingsManager;

  /**
   * The parsed querystring passed when starting the app, before any navigation
   * happens.
   */
  readonly initialRouteArgs: RouteArgs;

  /**
   * Returns the current trace object, if any. The instance being returned is
   * bound to the same plugin of App.pluginId.
   */
  readonly trace?: Trace;

  /**
   * Used to schedule things.
   */
  readonly raf: Raf;

  /**
   * Tracks async tasks for observability and idle detection.
   */
  readonly taskTracker: TaskTracker;

  /**
   * Manage the side panel tabs - a global side panel that appears on the right
   * of all pages, adding tabs and switching between them.
   *
   * @experimental - This is a new API and may change or be removed in the
   * future. Use with caution and be prepared for breaking changes.
   */
  readonly sidePanel: SidePanelManager;

  // True if the current user is an 'internal' user. E.g. a Googler on
  // ui.perfetto.dev. Plugins might use this to determine whether to show
  // certain internal links or expose certain experimental features by default.
  readonly isInternalUser: boolean;

  /**
   * Navigate to a new page.
   */
  navigate(newHash: string): void;

  /**
   * Returns the route/page we're on.
   */
  getCurrentRoute(): Route;

  openTraceFromFile(file: File): Promise<Trace>;
  openTraceFromUrl(url: string): Promise<Trace>;
  openTraceFromStream(stream: TraceStream): Promise<Trace>;
  openTraceFromBuffer(args: {
    buffer: ArrayBuffer;
    title: string;
    fileName: string;
  }): Promise<Trace>;
  closeCurrentTrace(): void;
}
