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

  // TODO(primiano): this should be needed in extremely rare cases. We should
  // probably switch to mithril auto-redraw at some point.
  scheduleFullRedraw(): void;

  /**
   * Navigate to a new page.
   */
  navigate(newHash: string): void;
}
