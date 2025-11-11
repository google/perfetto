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

  // True if the current user is an 'internal' user. E.g. a Googler on
  // ui.perfetto.dev. Plugins might use this to determine whether to show
  // certain internal links or expose certain experimental features by default.
  readonly isInternalUser: boolean;

  /**
   * Navigate to a new page.
   */
  navigate(newHash: string): void;

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
