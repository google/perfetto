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

import {DetailsPanel} from '../public/details_panel';
import {TabDescriptor, TabManager} from '../public/tab';
import {raf} from './raf_scheduler';

export interface ResolvedTab {
  uri: string;
  tab?: TabDescriptor;
}

export type TabPanelVisibility = 'COLLAPSED' | 'VISIBLE' | 'FULLSCREEN';

/**
 * Stores tab & current selection section registries.
 * Keeps track of tab lifecycles.
 */
export class TabManagerImpl implements TabManager, Disposable {
  private _registry = new Map<string, TabDescriptor>();
  private _defaultTabs = new Set<string>();
  private _detailsPanelRegistry = new Set<DetailsPanel>();
  private _instantiatedTabs = new Map<string, TabDescriptor>();
  private _openTabs: string[] = []; // URIs of the tabs open.
  private _currentTab: string = 'current_selection';
  private _tabPanelVisibility: TabPanelVisibility = 'COLLAPSED';
  private _tabPanelVisibilityChanged = false;

  [Symbol.dispose]() {
    // Dispose of all tabs that are currently alive
    for (const tab of this._instantiatedTabs.values()) {
      this.disposeTab(tab);
    }
    this._instantiatedTabs.clear();
  }

  registerTab(desc: TabDescriptor): Disposable {
    this._registry.set(desc.uri, desc);
    return {
      [Symbol.dispose]: () => this._registry.delete(desc.uri),
    };
  }

  addDefaultTab(uri: string): Disposable {
    this._defaultTabs.add(uri);
    return {
      [Symbol.dispose]: () => this._defaultTabs.delete(uri),
    };
  }

  registerDetailsPanel(section: DetailsPanel): Disposable {
    this._detailsPanelRegistry.add(section);
    return {
      [Symbol.dispose]: () => this._detailsPanelRegistry.delete(section),
    };
  }

  resolveTab(uri: string): TabDescriptor | undefined {
    return this._registry.get(uri);
  }

  showCurrentSelectionTab(): void {
    this.showTab('current_selection');
  }

  showTab(uri: string): void {
    // Add tab, unless we're talking about the special current_selection tab
    if (uri !== 'current_selection') {
      // Add tab to tab list if not already
      if (!this._openTabs.some((x) => x === uri)) {
        this._openTabs.push(uri);
      }
    }
    this._currentTab = uri;

    // The first time that we show a tab, auto-expand the tab bottom panel.
    // However, if the user has later collapsed the panel (hence if
    // _tabPanelVisibilityChanged == true), don't insist and leave things as
    // they are.
    if (
      !this._tabPanelVisibilityChanged &&
      this._tabPanelVisibility === 'COLLAPSED'
    ) {
      this.setTabPanelVisibility('VISIBLE');
    }

    raf.scheduleFullRedraw();
  }

  // Hide a tab in the tab bar pick a new tab to show.
  // Note: Attempting to hide the "current_selection" tab doesn't work. This tab
  // is special and cannot be removed.
  hideTab(uri: string): void {
    // If the removed tab is the "current" tab, we must find a new tab to focus
    if (uri === this._currentTab) {
      // Remember the index of the current tab
      const currentTabIdx = this._openTabs.findIndex((x) => x === uri);

      // Remove the tab
      this._openTabs = this._openTabs.filter((x) => x !== uri);

      if (currentTabIdx !== -1) {
        if (this._openTabs.length === 0) {
          // No more tabs, use current selection
          this._currentTab = 'current_selection';
        } else if (currentTabIdx < this._openTabs.length - 1) {
          // Pick the tab to the right
          this._currentTab = this._openTabs[currentTabIdx];
        } else {
          // Pick the last tab
          const lastTab = this._openTabs[this._openTabs.length - 1];
          this._currentTab = lastTab;
        }
      }
    } else {
      // Otherwise just remove the tab
      this._openTabs = this._openTabs.filter((x) => x !== uri);
    }
    raf.scheduleFullRedraw();
  }

  toggleTab(uri: string): void {
    return this.isOpen(uri) ? this.hideTab(uri) : this.showTab(uri);
  }

  isOpen(uri: string): boolean {
    return this._openTabs.find((x) => x == uri) !== undefined;
  }

  get currentTabUri(): string {
    return this._currentTab;
  }

  get openTabsUri(): string[] {
    return this._openTabs;
  }

  get tabs(): TabDescriptor[] {
    return Array.from(this._registry.values());
  }

  get defaultTabs(): string[] {
    return Array.from(this._defaultTabs);
  }

  get detailsPanels(): DetailsPanel[] {
    return Array.from(this._detailsPanelRegistry);
  }

  /**
   * Resolves a list of URIs to tabs and manages tab lifecycles.
   * @param tabUris List of tabs.
   * @returns List of resolved tabs.
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
      const oldTab = this._instantiatedTabs.get(uri);
      if (!oldTab) {
        this.initTab(tab);
      }
    }

    // Call onHide() on any tabs that have been removed.
    for (const [uri, tab] of this._instantiatedTabs) {
      const newTab = newTabs.get(uri);
      if (!newTab) {
        this.disposeTab(tab);
      }
    }

    this._instantiatedTabs = newTabs;

    return tabs;
  }

  setTabPanelVisibility(visibility: TabPanelVisibility): void {
    this._tabPanelVisibility = visibility;
    this._tabPanelVisibilityChanged = true;
    raf.scheduleFullRedraw();
  }

  toggleTabPanelVisibility(): void {
    switch (this._tabPanelVisibility) {
      case 'COLLAPSED':
      case 'FULLSCREEN':
        return this.setTabPanelVisibility('VISIBLE');
      case 'VISIBLE':
        this.setTabPanelVisibility('COLLAPSED');
        break;
    }
  }

  get tabPanelVisibility() {
    return this._tabPanelVisibility;
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
