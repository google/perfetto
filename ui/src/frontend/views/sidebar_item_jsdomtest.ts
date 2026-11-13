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

import m from 'mithril';
import {afterEach, beforeEach, describe, expect, test} from 'vitest';
import {AppImpl} from '../../core/app_impl';
import {createFakeTraceImpl} from '../../core/fake_trace_impl';
import type {SidebarMenuItemInternal} from '../../core/sidebar_manager';
import {SidebarItem} from './sidebar_item';

let container: HTMLElement;

beforeEach(() => {
  createFakeTraceImpl();
  container = document.createElement('div');
  document.body.appendChild(container);
});

afterEach(() => {
  m.render(container, null);
  container.remove();
});

function renderItems(items: SidebarMenuItemInternal[]) {
  m.render(
    container,
    m(
      'ul',
      items.map((item) => m(SidebarItem, {app: AppImpl.instance, item})),
    ),
  );
}

function renderedTexts(): string[] {
  return Array.from(container.querySelectorAll('li')).map(
    (li) => li.textContent ?? '',
  );
}

describe('SidebarItem', () => {
  test('leaves out hidden items', () => {
    let hidden = true;
    const items: SidebarMenuItemInternal[] = [
      {id: 'a', section: 'support', text: 'Shown', action: () => {}},
      {
        id: 'b',
        section: 'support',
        text: 'Sometimes',
        action: () => {},
        hidden: () => hidden,
      },
      {
        id: 'c',
        section: 'support',
        text: 'Never',
        action: () => {},
        hidden: true,
      },
    ];

    renderItems(items);
    expect(renderedTexts()).toEqual(['Shown']);

    hidden = false;
    renderItems(items);
    expect(renderedTexts()).toEqual(['Shown', 'Sometimes']);
  });

  test('reads an href callback on every render', () => {
    let url = 'https://example.com/a';
    const item: SidebarMenuItemInternal = {
      id: 'a',
      section: 'support',
      text: 'Link',
      href: () => url,
    };
    const href = () => container.querySelector('a')?.getAttribute('href');

    renderItems([item]);
    expect(href()).toBe('https://example.com/a');

    url = 'https://example.com/b';
    renderItems([item]);
    expect(href()).toBe('https://example.com/b');
  });
});
