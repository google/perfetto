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
import type {Analytics} from '../public/analytics';
import {PageManagerImpl} from './page_manager';

const analytics: Analytics = {
  logEvent: () => {},
  logError: () => {},
  isEnabled: () => false,
};

let container: HTMLElement;

beforeEach(() => {
  container = document.createElement('div');
  document.body.appendChild(container);
});

afterEach(() => {
  m.render(container, null);
  container.remove();
  window.location.hash = '';
});

describe('PageManagerImpl', () => {
  function createPages(): PageManagerImpl {
    const pages = new PageManagerImpl(analytics);
    pages.registerPage({route: '/', render: () => m('.home')});
    pages.registerPage({
      route: '/foo',
      render: (subpage) => m('.foo', subpage),
    });
    pages.registerPage({route: '/empty', render: () => undefined});
    return pages;
  }

  // Navigates to a route and renders the pages the manager returns.
  function renderRoute(pages: PageManagerImpl, hash: string) {
    window.location.hash = hash;
    m.render(container, pages.renderPageForCurrentRoute());
  }

  function getOpenPage(): Element | null {
    return container.querySelector('[data-gate-open="true"]');
  }

  test('shows the page for the current route', () => {
    const pages = createPages();
    renderRoute(pages, '#!/foo/bar');
    expect(getOpenPage()?.querySelector('.foo')?.textContent).toBe('/bar');
  });

  test('shows the home page for a route that is not registered', () => {
    const pages = createPages();
    renderRoute(pages, '#!/missing');
    expect(getOpenPage()?.querySelector('.home')).not.toBeNull();
  });

  test('shows the home page when the route renders nothing', () => {
    const pages = createPages();
    renderRoute(pages, '#!/empty');
    expect(getOpenPage()?.querySelector('.home')).not.toBeNull();
  });

  test('keeps pages visited earlier mounted but closed', () => {
    const pages = createPages();
    renderRoute(pages, '#!/foo');
    renderRoute(pages, '#!/');

    const gates = Array.from(container.querySelectorAll('[data-gate-open]'));
    expect(gates.map((gate) => gate.getAttribute('data-gate-open'))).toEqual([
      'false',
      'true',
    ]);
    expect(getOpenPage()?.querySelector('.home')).not.toBeNull();
  });
});
