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

import type {
  SidePanelManager,
  SidePanelTabDescriptor,
} from '../public/side_panel';

export class SidePanelManagerImpl implements SidePanelManager {
  // The registry of tabs.
  private _registry = new Map<string, SidePanelTabDescriptor>();

  // The URI of the current tab we're looking at.
  private _currentTabUri: string | undefined;

  // Whether the entire side panel is visible or not.
  private _visible = false;

  registerTab(desc: SidePanelTabDescriptor): Disposable {
    this._registry.set(desc.uri, desc);
    return {
      [Symbol.dispose]: () => {
        this._registry.delete(desc.uri);
        if (this._currentTabUri === desc.uri) {
          this._currentTabUri = undefined;
        }
      },
    };
  }

  showTab(uri: string): void {
    if (!this._registry.has(uri)) {
      console.warn(
        `Trying to show side panel tab with URI ${uri} that is not registered`,
      );
      return;
    }
    this._currentTabUri = uri;
    this._visible = true;
  }

  get visible(): boolean {
    return this._visible;
  }

  set visible(v: boolean) {
    this._visible = v;
  }

  get currentTabUri(): string | undefined {
    return this._currentTabUri;
  }

  resolveTab(uri: string): SidePanelTabDescriptor | undefined {
    return this._registry.get(uri);
  }

  get registeredTabs(): ReadonlyArray<SidePanelTabDescriptor> {
    return Array.from(this._registry.values());
  }
}
