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
import {prettyDOM} from '@testing-library/dom';
import {Tabs} from './tabs';

// DOM tests for the `lazy` option on TabsTab: a lazy tab's content must not
// be rendered until the tab is first activated, and must stay mounted (like
// regular tab content) once it has been.
//
// The tests render Tabs into `container` (jsdom, set globally in
// vitest.config.mjs) via m.render and query the resulting DOM. We render
// manually, so a `rerender` thunk flushes after interactions (m.redraw is a
// no-op here). Failures attach a prettyDOM() dump as the assertion message.

let container: HTMLElement;

beforeEach(() => {
  container = document.createElement('div');
  document.body.appendChild(container);
});

afterEach(() => {
  m.render(container, null);
  container.remove();
});

// Failure message: a pretty-printed dump of the rendered tabs, so a mismatch
// shows the actual DOM rather than just a null element.
function dumpDom(): string {
  const out = prettyDOM(container);
  return typeof out === 'string' ? out : '';
}

// Renders a controlled Tabs with two tabs: an active, non-lazy 'Tab A' and an
// inactive, lazy 'Tab B'. Returns helpers to flush renders and to activate a
// tab by clicking its handle.
function renderTabs() {
  let activeKey = 'a';
  const rerender = () =>
    m.render(
      container,
      m(Tabs, {
        activeTabKey: activeKey,
        onTabChange: (key: string) => {
          activeKey = key;
        },
        tabs: [
          {key: 'a', title: 'Tab A', content: m('.tab-a-content', 'A')},
          {
            key: 'b',
            title: 'Tab B',
            lazy: true,
            content: m('.tab-b-content', 'B'),
          },
        ],
      }),
    );

  // Activates a tab by pointerdown on its handle, as the Tabs component
  // listens for pointerdown (not click) to switch tabs.
  const activate = (title: string) => {
    const handle = Array.from(
      container.querySelectorAll<HTMLElement>('.pf-tabs__tab'),
    ).find((el) => el.textContent?.trim() === title);
    expect(handle, dumpDom()).toBeTruthy();
    handle!.dispatchEvent(new Event('pointerdown', {bubbles: true}));
    rerender();
  };

  rerender();
  return {rerender, activate};
}

describe('Tabs lazy content', () => {
  test('lazy tab content is not rendered until the tab is activated', () => {
    const {activate} = renderTabs();
    // The active non-lazy tab renders its content eagerly...
    expect(container.querySelector('.tab-a-content'), dumpDom()).toBeTruthy();
    // ...while the inactive lazy tab renders none.
    expect(container.querySelector('.tab-b-content'), dumpDom()).toBeNull();

    activate('Tab B');
    expect(container.querySelector('.tab-b-content'), dumpDom()).toBeTruthy();
  });

  test('lazy tab content stays mounted after the tab is deactivated', () => {
    const {activate} = renderTabs();
    activate('Tab B');
    expect(container.querySelector('.tab-b-content'), dumpDom()).toBeTruthy();

    activate('Tab A');
    // Switching away does not unmount the content: the tab has been
    // activated, so it now behaves like a regular (gated) tab.
    expect(container.querySelector('.tab-b-content'), dumpDom()).toBeTruthy();
  });
});
