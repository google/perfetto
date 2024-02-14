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

import {Disposable, DisposableCallback} from '../base/disposable';
import {DetailsPanel, TabDescriptor} from '../public';

export interface ResolvedTab {
  uri: string;
  tab?: TabDescriptor;
}

/**
 * Stores tab & current selection section registries.
 * Keeps track of tab lifecycles.
 */
export class TabManager implements Disposable {
  private _registry = new Map<string, TabDescriptor>();
  private _defaultTabs = new Set<string>();
  private _detailsPanelsRegistry = new Set<DetailsPanel>();
  private _currentTabs = new Map<string, TabDescriptor>();

  dispose(): void {
    // Dispose of all tabs that are currently alive
    for (const tab of this._currentTabs.values()) {
      this.disposeTab(tab);
    }
    this._currentTabs.clear();
  }

  registerTab(desc: TabDescriptor): Disposable {
    this._registry.set(desc.uri, desc);
    return new DisposableCallback(() => {
      this._registry.delete(desc.uri);
    });
  }

  addDefaultTab(uri: string): Disposable {
    this._defaultTabs.add(uri);
    return new DisposableCallback(() => {
      this._defaultTabs.delete(uri);
    });
  }

  registerDetailsPanel(section: DetailsPanel): Disposable {
    this._detailsPanelsRegistry.add(section);
    return new DisposableCallback(() => {
      this._detailsPanelsRegistry.delete(section);
    });
  }

  resolveTab(uri: string): TabDescriptor|undefined {
    return this._registry.get(uri);
  }

  get tabs(): TabDescriptor[] {
    return Array.from(this._registry.values());
  }

  get defaultTabs(): string[] {
    return Array.from(this._defaultTabs);
  }

  get detailsPanels(): DetailsPanel[] {
    return Array.from(this._detailsPanelsRegistry);
  }

  /**
   * Resolves a list of URIs to tabs and manages tab lifecycles.
   * @param tabUris List of tabs.
   * @return List of resolved tabs.
   */
  resolveTabs(tabUris: string[]): ResolvedTab[] {
    // Refresh the list of old tabs
    const newTabs = new Map<string, TabDescriptor>();
    const tabs: ResolvedTab[] = [];

    tabUris.forEach((uri) => {
      const newTab = this._registry.get(uri);
      tabs.push({uri, tab: newTab});

      if (newTab) {
        newTabs.set(uri, newTab);
      }
    });

    // Call onShow() on any new tabs.
    for (const [uri, tab] of newTabs) {
      const oldTab = this._currentTabs.get(uri);
      if (!oldTab) {
        this.initTab(tab);
      }
    }

    // Call onHide() on any tabs that have been removed.
    for (const [uri, tab] of this._currentTabs) {
      const newTab = newTabs.get(uri);
      if (!newTab) {
        this.disposeTab(tab);
      }
    }

    this._currentTabs = newTabs;

    return tabs;
  }

  /**
   * Call onShow() on this tab.
   * @param tab The tab to initialize.
   */
  private initTab(tab: TabDescriptor): void {
    tab.onShow?.();
  }

  /**
   * Call onHide() and maybe remove from registry if tab is ephemeral.
   * @param tab The tab to dispose.
   */
  private disposeTab(tab: TabDescriptor): void {
    // Attempt to call onHide
    tab.onHide?.();

    // If ephemeral, also unregister the tab
    if (tab.isEphemeral) {
      this._registry.delete(tab.uri);
    }
  }
}
