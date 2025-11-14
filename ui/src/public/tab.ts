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

import m from 'mithril';

/**
 * Manages the registration, display, and hiding of tabs within the UI.
 *
 * Tabs provide a way to organize different views or functionalities within
 * the application, allowing users to switch between them easily.
 */
export interface TabManager {
  /**
   * Registers a new tab with the TabManager.
   *
   * @param tab The descriptor for the tab to register.
   */
  registerTab(tab: TabDescriptor): void;

  /**
   * Displays the tab associated with the given URI.
   *
   * If the tab is not currently visible, it will be brought to the foreground.
   * @param uri The unique URI of the tab to show.
   */
  showTab(uri: string): void;

  /**
   * Hides the tab associated with the given URI.
   *
   * @param uri The unique URI of the tab to hide.
   */
  hideTab(uri: string): void;

  /**
   * Adds a tab to the list of default tabs.
   *
   * Default tabs are automatically opened when the application starts or
   * when a new trace is loaded.
   * @param uri The unique URI of the tab to add as a default.
   */
  addDefaultTab(uri: string): void;
}

/**
 * Represents the content and title of a tab.
 */
export interface Tab {
  /**
   * Renders the content of the tab.
   * @returns The Mithril children to render for the tab's content.
   */
  render(): m.Children;

  /**
   * Gets the title of the tab.
   * @returns The human-readable title of the tab.
   */
  getTitle(): string;
}

/**
 * Describes a tab to be registered with the TabManager.
 */
export interface TabDescriptor {
  /**
   * The unique URI for this tab.
   * TODO(stevegolton): Maybe optional for ephemeral tabs.
   */
  readonly uri: string;
  /**
   * The content of the tab, including its render function and title.
   */
  readonly content: Tab;
  /**
   * If true, this tab is ephemeral and may be closed automatically under
   * certain conditions (e.g., when a trace is closed). Defaults to `false`.
   */
  readonly isEphemeral?: boolean;
  /**
   * An optional callback function that is invoked when the tab is hidden.
   */
  onHide?(): void;
  /**
   * An optional callback function that is invoked when the tab is shown.
   */
  onShow?(): void;
}
