// Copyright (C) 2026 The Android Open Source Project
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

import type m from 'mithril';

export interface SidePanelTabDescriptor {
  readonly uri: string;
  readonly title: string;
  readonly icon?: string;
  render(): m.Children;
}

export interface SidePanelManager {
  /**
   * Register a tab in the side panel.
   *
   * The tab becomes available in the side panel's tab list, keyed by its
   * `uri`. Registering a tab does not make the side panel visible, nor does
   * it switch to the new tab - use {@link showTab} for that. Disposing the
   * returned `Disposable` unregisters the tab; if it was the currently
   * displayed tab, the side panel clears its current selection (but stays
   * open).
   *
   * @experimental - This is a new API and may change or be removed in the
   * future. Use with caution and be prepared for breaking changes.
   */
  registerTab(tab: SidePanelTabDescriptor): Disposable;

  /**
   * Show a given tab in the side panel.
   *
   * Switches the side panel to the tab with the given `uri` and makes the
   * side panel visible if it isn't already. If no tab with that uri is
   * registered, this is a no-op (a warning is logged).
   *
   * @experimental - This is a new API and may change or be removed in the
   * future. Use with caution and be prepared for breaking changes.
   */
  showTab(uri: string): void;
}
