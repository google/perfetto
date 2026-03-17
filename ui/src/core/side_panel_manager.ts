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

import {SidePanelManager, SidePanelTabDescriptor} from '../public/side_panel';

export class SidePanelManagerImpl implements SidePanelManager {
  private _registry = new Map<string, SidePanelTabDescriptor>();
  private _openTabs: string[] = [];
  private _currentTab: string | undefined;
  private _visible = false;

  registerTab(desc: SidePanelTabDescriptor): Disposable {
    this._registry.set(desc.uri, desc);
    return {
      [Symbol.dispose]: () => {
        this._registry.delete(desc.uri);
        this.hideTab(desc.uri);
      },
    };
  }

  showTab(uri: string): void {
    if (!this._openTabs.includes(uri)) {
      this._openTabs.push(uri);
    }
    this._currentTab = uri;
    this._visible = true;
  }

  hideTab(uri: string): void {
    this._openTabs = this._openTabs.filter((t) => t !== uri);
    if (this._currentTab === uri) {
      this._currentTab = this._openTabs[this._openTabs.length - 1];
      if (!this._currentTab) {
        this._visible = false;
      }
    }
  }

  get visible(): boolean {
    return this._visible;
  }

  set visible(v: boolean) {
    this._visible = v;
  }

  get currentTabUri(): string | undefined {
    return this._currentTab;
  }

  get openTabs(): ReadonlyArray<string> {
    return this._openTabs;
  }

  resolveTab(uri: string): SidePanelTabDescriptor | undefined {
    return this._registry.get(uri);
  }

  get registeredTabs(): ReadonlyArray<SidePanelTabDescriptor> {
    return Array.from(this._registry.values());
  }
}
