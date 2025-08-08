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

/**
 * The API endpoint to interact programmaticaly with the UI before a trace has
 * been loaded. This is passed to plugins' OnActivate().
 */
export interface App {
  /**
   * The unique id for this plugin (as specified in the PluginDescriptor),
   * or '__core__' for the interface exposed to the core.
   */
  readonly pluginId: string;
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
   * Args in the URL bar that start with this plugin's id.
   */
  readonly initialPluginRouteArgs: {[key: string]: number | boolean | string};

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
   * Navigate to a new page.
   */
  navigate(newHash: string): void;

  openTraceFromFile(file: File): void;
  openTraceFromUrl(url: string): void;
  openTraceFromBuffer(args: {
    buffer: ArrayBuffer;
    title: string;
    fileName: string;
  }): void;
  closeCurrentTrace(): void;
}
