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
import type {Policy} from './policy';
import type {Raf} from './raf';
import type {RouteArgs} from './route_schema';
import type {SettingsManager} from './settings';
import type {SidePanelManager} from './side_panel';
import type {SidebarManager} from './sidebar';
import type {SqlPackage} from './extra_sql_packages';
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

export type TraceLoadPhase =
  | 'downloading'
  | 'initializing'
  | 'parsing'
  | 'indexing'
  | 'plugins'
  | 'finalizing';

export interface TraceLoadProgress {
  readonly title: string;
  readonly phase: TraceLoadPhase;
  readonly status: string;
  // Overall progress percentage in [0, 100], or undefined when indeterminate.
  readonly pct?: number;
  readonly bytesRead?: number;
  readonly bytesTotal?: number;
  readonly mbPerSec?: number;
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
   * True while a trace is being downloaded, parsed, or initialized.
   */
  readonly isLoadingTrace: boolean;

  /**
   * Structured progress of the in-flight trace load, if any.
   */
  readonly traceLoadProgress?: TraceLoadProgress;

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

  /**
   * Which additional features are on for the current user.
   */
  readonly policy: Policy;

  // The standalone product this UI is running as (e.g. 'memscope'), or
  // undefined for the general purpose Perfetto UI. See products/README.md.
  readonly productId: string | undefined;

  /**
   * Navigate to a new page.
   */
  navigate(newHash: string): void;

  /**
   * Returns the route/page we're on.
   */
  getCurrentRoute(): Route;

  /**
   * Registers PerfettoSQL packages into the engine of every trace loaded
   * after this call, so queries can `INCLUDE PERFETTO MODULE <pkg>.<module>`.
   */
  addSqlPackages(
    args: ReadonlyArray<SqlPackage> | Promise<ReadonlyArray<SqlPackage>>,
  ): void;

  openTraceFromFile(file: File): Promise<Trace>;
  openTraceFromUrl(url: string): Promise<Trace>;
  openTraceFromStream(stream: TraceStream): Promise<Trace>;
  openTraceFromBuffer(args: {
    buffer: ArrayBuffer;
    title: string;
    fileName: string;

    // Extra arguments for plugins, passed to their onTraceLoad(), keyed by
    // plugin id. E.g. {'com.example.Foo': {key1: 'value1'}}.
    pluginArgs?: {[pluginId: string]: {[key: string]: unknown}};

    // Whether the UI may share the trace externally (e.g. as a permalink) or
    // download it to disk. Both default to true.
    shareable?: boolean;
    downloadable?: boolean;

    // Whether the trace may be stored in the browser cache so that reloading
    // the page reopens it. Defaults to true. Set it to false for traces that
    // the opener regenerates on reload.
    cacheable?: boolean;
  }): Promise<Trace>;
  closeCurrentTrace(): void;
}
