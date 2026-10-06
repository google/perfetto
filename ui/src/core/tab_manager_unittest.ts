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

import {DrawerPanelVisibility} from '../widgets/drawer_panel';
import {TabManagerImpl} from './tab_manager';

describe('TabManagerImpl', () => {
  let tabs: TabManagerImpl;
  let numChanges: number;

  beforeEach(() => {
    tabs = new TabManagerImpl();
    numChanges = 0;
    tabs.onTabPanelStateChanged.addListener(() => numChanges++);
  });

  test('current selection tab is hidden while the panel is collapsed', () => {
    expect(tabs.isCurrentSelectionTabVisible).toBe(false);
    tabs.showCurrentSelectionTab();
    expect(tabs.isCurrentSelectionTabVisible).toBe(true);
    expect(numChanges).toBeGreaterThan(0);

    numChanges = 0;
    tabs.setTabPanelVisibility(DrawerPanelVisibility.COLLAPSED);
    expect(tabs.isCurrentSelectionTabVisible).toBe(false);
    expect(numChanges).toBe(1);

    tabs.setTabPanelVisibility(DrawerPanelVisibility.FULLSCREEN);
    expect(tabs.isCurrentSelectionTabVisible).toBe(true);
  });

  test('current selection tab is hidden when another tab is shown', () => {
    tabs.showCurrentSelectionTab();
    numChanges = 0;

    tabs.showTab('some_tab');
    expect(tabs.isCurrentSelectionTabVisible).toBe(false);
    expect(numChanges).toBe(1);

    // Closing the only other tab falls back to the current selection tab.
    tabs.hideTab('some_tab');
    expect(tabs.isCurrentSelectionTabVisible).toBe(true);
    expect(numChanges).toBe(2);
  });
});
