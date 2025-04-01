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
 * Manages the state for a simple status bar that can display one piece of content at a time.
 *
 * At least for now, this is not meant to be used by several plugins at the same time.
 */

export interface StatusBarItem {
  readonly renderItem: () => {
    readonly label: string;
    readonly icon?: string;
    readonly intent?: Intent;
    readonly onclick?: (event: MouseEvent) => void;
  };
  readonly popupContent?: () => m.Children;
}

export class StatusBarManager {
  private _statusBarItems: StatusBarItem[] = [];

  registerItem(item: StatusBarItem) {
    this._statusBarItems.push(item);
  }

  get statusBarItems(): ReadonlyArray<StatusBarItem> {
    return this._statusBarItems;
  }
}
