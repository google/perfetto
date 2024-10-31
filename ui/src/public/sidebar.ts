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

// Indicates the potential enabled states the sidebar can be in.
export type SidebarEnabled = 'ENABLED' | 'DISABLED';

// Indicates the potential visibility states the sidebar can be in.
export type SidebarVisibility = 'HIDDEN' | 'VISIBLE';

export interface SidebarManager {
  readonly sidebarEnabled: SidebarEnabled;

  /**
   * Adds a new menu item to the sidebar.
   * All entries must map to a command. This will allow the shortcut and
   * optional shortcut to be displayed on the UI.
   */
  addMenuItem(menuItem: SidebarMenuItem): void;

  /**
   * Gets the current visibility of the sidebar.
   */
  get sidebarVisibility(): SidebarVisibility;

  /**
   * Toggles the visibility of the sidebar. Can only be called when
   * `sidebarEnabled` returns `ENABLED`.
   */
  toggleSidebarVisbility(): void;
}

export interface SidebarMenuItem {
  readonly commandId: string;
  readonly group:
    | 'navigation'
    | 'current_trace'
    | 'convert_trace'
    | 'example_traces'
    | 'support';
  when?(): boolean;
  readonly icon: string;
  readonly priority?: number;
}
