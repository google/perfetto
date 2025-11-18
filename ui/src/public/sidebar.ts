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

import {Icons} from '../base/semantic_icons';

// For now sections are fixed and cannot be extended by plugins.
export const SIDEBAR_SECTIONS = {
  navigation: {
    title: 'Navigation',
    summary: 'Open or record a new trace',
    icon: 'source',
  },
  current_trace: {
    title: 'Current Trace',
    summary: 'Actions on the current trace',
    icon: Icons.ShowChart,
  },
  example_traces: {
    title: 'Example Traces',
    summary: 'Open an example trace',
    icon: Icons.Description,
  },
  settings: {
    title: 'Settings',
    summary: 'Preferences and configuration',
    icon: Icons.Settings,
  },
  support: {
    title: 'Support',
    summary: 'Documentation & Bugs',
    icon: Icons.Help,
  },
  convert_trace: {
    title: 'Convert trace',
    summary: 'Convert to other formats',
    icon: Icons.Transform,
  },
} as const;

export type SidebarSections = keyof typeof SIDEBAR_SECTIONS;

export interface SidebarManager {
  readonly enabled: boolean;

  /**
   * Adds a new menu item to the sidebar.
   * All entries must map to a command. This will allow the shortcut and
   * optional shortcut to be displayed on the UI.
   */
  addMenuItem(menuItem: SidebarMenuItem): void;

  /**
   * Gets the current collapsed state of the sidebar.
   * When collapsed, the sidebar shows icons only.
   */
  get collapsed(): boolean;

  /**
   * Toggles the collapsed state of the sidebar.
   * Can only be called when `sidebarEnabled` returns `ENABLED`.
   */
  toggleCollapsed(): void;
}

export type SidebarMenuItem = {
  readonly section: SidebarSections;
  readonly sortOrder?: number;

  // The properties below can be mutated by passing a callback rather than a
  // direct value. The callback is invoked on every render frame, keep it cheap.
  // readonly text: string | (() => string);
  readonly icon?: string | (() => string);
  readonly tooltip?: string | (() => string);
  readonly cssClass?: string | (() => string); // Without trailing '.'.

  // If false or omitted the item works normally.
  // If true the item is striken through and the action/href will be a no-op.
  // If a string, the item acts as disabled and clicking on it shows a popup
  // that shows the returned text (the string has "disabled reason" semantic);
  readonly disabled?: string | boolean | (() => string | boolean);

  // One of the three following arguments must be specified.
} & (
  | {
      /** The text of the menu entry. Required. */
      readonly text: string | (() => string);

      /**
       * The URL to navigate to. It can be either:
       * - A local route (e.g. ''#!/query').
       * - An absolute URL (e.g. 'https://example.com'). In this case the link will
       *   be open in a target=_blank new tag.
       */
      readonly href: string;
    }
  | {
      /** The text of the menu entry. Required. */
      readonly text: string | (() => string);

      /**
       * The function that will be invoked when clicking. If the function returns
       * a promise, a spinner will be drawn next to the sidebar entry until the
       * promise resolves.
       */
      readonly action: () => unknown | Promise<unknown>;

      /** Optional. If omitted href = '#'. */
      readonly href?: string;
    }
  | {
      /** Optional. If omitted uses the command name. */
      readonly text?: string | (() => string);

      /** The ID of the command that will be invoked when clicking */
      readonly commandId: string;
    }
);
