// Copyright (C) 2024 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

import {SidebarMenuItem} from '../public/sidebar';
import {SidebarManagerImpl} from './sidebar_manager';

const TestMenuItem = (text: string): SidebarMenuItem => ({
  section: 'current_trace',
  text,
  action: jest.fn(),
});

describe('SidebarManagerImpl child manager', () => {
  test('child registry sees parent items but not vice versa', () => {
    const parent = new SidebarManagerImpl();
    parent.addMenuItem(TestMenuItem('parent'));
    const child = parent.createChild('test');
    // Child sees parent's item - get id
    const parentItem = Array.from(parent.menuItems.values())[0];
    expect(child.menuItems.has(parentItem.id)).toBe(true);
    expect(child.menuItems.get(parentItem.id)).toBe(parentItem);
    // Add to child, parent does not see it
    child.addMenuItem(TestMenuItem('child'));
    const childItem = Array.from(child.menuItems.values()).find((i) => i.text === 'child');
    expect(childItem).toBeDefined();
    expect(parent.menuItems.has(childItem!.id)).toBe(false);
  });
});
