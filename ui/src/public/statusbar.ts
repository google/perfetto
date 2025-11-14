// Copyright (C) 2025 The Android Open Source Project
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
import {Intent} from '../widgets/common';

/**
 * Represents an item to be displayed in the status bar.
 */
export interface StatusbarItem {
  /**
   * A function that returns the properties for rendering the status bar item.
   * @returns An object with label, optional icon, optional intent, and an
   *   optional click handler.
   */
  renderItem(): {
    readonly label: string;
    readonly icon?: string;
    readonly intent?: Intent;
    onclick?(event: MouseEvent): void;
  };

  /**
   * An optional function that returns the content to be displayed in a popup
   * when the status bar item is clicked.
   * @returns The Mithril children to render in the popup.
   */
  popupContent?(): m.Children;
}

/**
 * Manages items in the status bar.
 */
export interface StatusbarManager {
  /**
   * A read-only array of all currently registered status bar items.
   */
  readonly statusBarItems: ReadonlyArray<StatusbarItem>;

  /**
   * Registers a new item to be displayed in the status bar.
   * @param item The status bar item to register.
   */
  registerItem(item: StatusbarItem): void;
}
