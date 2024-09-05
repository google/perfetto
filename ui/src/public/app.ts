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

import {Command} from './command';
import {SidebarMenuItem} from './sidebar';

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

  commands: {
    registerCommand(command: Command): void;

    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    runCommand(id: string, ...args: any[]): any;
  };

  sidebar: {
    /**
     * Adds a new menu item to the sidebar.
     * All entries must map to a command. This will allow the shortcut and
     * optional shortcut to be displayed on the UI.
     */
    addSidebarMenuItem(menuItem: SidebarMenuItem): void;
  };
}
