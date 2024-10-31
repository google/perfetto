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

import {Registry} from '../base/registry';
import {
  SidebarEnabled,
  SidebarManager,
  SidebarMenuItem,
  SidebarVisibility,
} from '../public/sidebar';
import {raf} from './raf_scheduler';

export class SidebarManagerImpl implements SidebarManager {
  private _sidebarVisibility: SidebarVisibility;
  readonly menuItems = new Registry<SidebarMenuItem>((m) => m.commandId);

  constructor(public readonly sidebarEnabled: SidebarEnabled) {
    this._sidebarVisibility =
      sidebarEnabled === 'ENABLED' ? 'VISIBLE' : 'HIDDEN';
  }

  addMenuItem(menuItem: SidebarMenuItem): Disposable {
    return this.menuItems.register(menuItem);
  }

  public get sidebarVisibility() {
    return this._sidebarVisibility;
  }

  public toggleSidebarVisbility() {
    if (this._sidebarVisibility === 'HIDDEN') {
      this._sidebarVisibility = 'VISIBLE';
    } else {
      this._sidebarVisibility = 'HIDDEN';
    }
    raf.scheduleFullRedraw();
  }
}
