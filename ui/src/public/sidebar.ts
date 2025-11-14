// Copyright (C) 2024 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-20.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// For now sections are fixed and cannot be extended by plugins.
export const SIDEBAR_SECTIONS = {
  navigation: {
    title: 'Navigation',
    summary: 'Open or record a new trace',
  },
  current_trace: {
    title: 'Current Trace',
    summary: 'Actions on the current trace',
  },
  example_traces: {
    title: 'Example Traces',
    summary: 'Open an example trace',
  },
  settings: {
    title: 'Settings',
    summary: 'Preferences and configuration',
  },
  support: {
    title: 'Support',
    summary: 'Documentation & Bugs',
  },
  convert_trace: {
    title: 'Convert trace',
    summary: 'Convert to other formats',
  },
} as const;

export type SidebarSections = keyof typeof SIDEBAR_SECTIONS;

/**
 * Manages the sidebar menu items and visibility.
 *
 * The sidebar is organized into sections (navigation, settings, support,
 * etc.). Use this to add menu entries that can navigate to pages, execute
 * actions, or trigger commands. Menu items are automatically removed when
 * the trace is closed or the plugin is unloaded.
 */
export interface SidebarManager {
  /**
   * Whether the sidebar is currently enabled.
   *
   * When the sidebar is disabled, menu items cannot be added, and its
   * visibility cannot be toggled.
   */
  readonly enabled: boolean;

  /**
   * Adds a new menu item to the sidebar.
   *
   * All entries must map to a command, which allows the shortcut and
   * optional shortcut to be displayed on the UI.
   *
   * @param menuItem The menu item to add.
   */
  addMenuItem(menuItem: SidebarMenuItem): void;

  /**
   * Gets the current visibility of the sidebar.
   *
   * @returns `true` if the sidebar is visible, `false` otherwise.
   */
  get visible(): boolean;

  /**
   * Toggles the visibility of the sidebar.
   *
   * This method can only be called when `enabled` is `true`.
   */
  toggleVisibility(): void;
}

/**
 * Represents a single menu item in the sidebar.
 *
 * A sidebar menu item can either navigate to a URL, execute an action, or
 * trigger a command.
 */
export type SidebarMenuItem = {
  /**
   * The section of the sidebar where this menu item will be placed.
   *
   * Must be one of the predefined `SIDEBAR_SECTIONS`.
   */
  readonly section: SidebarSections;

  /**
   * An optional number to control the sort order of menu items within a
   * section.
   *
   * Lower numbers appear before higher numbers. If omitted, items are sorted
   * by their `text` property.
   */
  readonly sortOrder?: number;

  /**
   * An optional icon to display next to the menu item.
   *
   * Can be a string (e.g., 'settings') or a function that returns a string.
   * The function is invoked on every render frame, so keep it cheap.
   */
  readonly icon?: string | (() => string);

  /**
   * An optional tooltip to display when hovering over the menu item.
   *
   * Can be a string or a function that returns a string. The function is
   * invoked on every render frame, so keep it cheap.
   */
  readonly tooltip?: string | (() => string);

  /**
   * An optional CSS class to apply to the menu item.
   *
   * Can be a string (without trailing '.') or a function that returns a string.
   * The function is invoked on every render frame, so keep it cheap.
   */
  readonly cssClass?: string | (() => string); // Without trailing '.'.

  /**
   * Controls the disabled state of the menu item.
   *
   * - If `false` or omitted, the item works normally.
   * - If `true`, the item is struck through, and its action/href will be a
   *   no-op.
   * - If a `string`, the item acts as disabled, and clicking on it shows a
   *   popup with the returned text (which has "disabled reason" semantics).
   *
   * Can be a string, boolean, or a function that returns either. The function
   * is invoked on every render frame, so keep it cheap.
   */
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
      action(): unknown | Promise<unknown>;

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
