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
import {SidebarManager, SidebarMenuItem} from '../public/sidebar';
import {raf} from './raf_scheduler';

export class SidebarManagerImpl implements SidebarManager {
  private _sidebarHidden = false;
  readonly menuItems = new Registry<SidebarMenuItem>((m) => m.commandId);

  constructor(sidebarHidden?: boolean) {
    this._sidebarHidden = sidebarHidden ?? false;
  }

  addMenuItem(menuItem: SidebarMenuItem): Disposable {
    return this.menuItems.register(menuItem);
  }

  get sidebarHidden() {
    return this._sidebarHidden;
  }

  set sidebarHidden(value: boolean) {
    this._sidebarHidden = value;
    raf.scheduleFullRedraw();
  }
}
