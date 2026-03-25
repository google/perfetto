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

// Keep this import first.
import '../base/static_initializers';
import m from 'mithril';
import {defer} from '../base/deferred';
import {reportError, addErrorHandler, ErrorDetails} from '../base/logging';
import {initLiveReload} from '../core/live_reload';
import {settingsStorage} from './settings/settings_storage';
import {ThemeProvider} from '../frontend/theme_provider';
import {OverlayContainer} from '../widgets/overlay_container';
import {QueryPage} from './pages/query_page';
import {HomePage} from './pages/home_page';
import {bigTraceSettingsStorage} from './settings/bigtrace_settings_storage';
import {queryState} from './query/query_state';
import {SettingsPage} from './pages/settings_page';
import {Topbar} from './layout/topbar';
import {Sidebar, SidebarMenuItem, SIDEBAR_SECTIONS} from './layout/sidebar';

function getRoot() {
  // Works out the root directory where the content should be served from
  // e.g. `http://origin/v1.2.3/`.
  const script = document.currentScript as HTMLScriptElement;

  // Needed for DOM tests, that do not have script element.
  if (script === null) {
    return '';
  }

  let root = script.src;
  root = root.substr(0, root.lastIndexOf('/') + 1);
  return root;
}

function setupContentSecurityPolicy() {
  // Note: self and sha-xxx must be quoted, urls data: and blob: must not.
  const policy = {
    'default-src': [`'self'`],
    'script-src': [`'self'`],
    'object-src': ['none'],
    'connect-src': [
      `'self'`,
      'https://autopush-brush-googleapis.corp.google.com',
      'https://brush-googleapis.corp.google.com',
    ],
    'img-src': [`'self'`, 'data:', 'blob:'],
    'style-src': [
      `'self'`,
      `'sha256-47DEQpj8HBSa+/TImW+5JCeuQeRkm5NMpJWZG3hSuFU='`,
      `'sha256-yRQRG6LLKMvjvigtzXD1f8VRZSYY7J8fM2ZLfdMaHKg='`,
    ],
  };
  const meta = document.createElement('meta');
  meta.httpEquiv = 'Content-Security-Policy';
  let policyStr = '';
  for (const [key, list] of Object.entries(policy)) {
    policyStr += `${key} ${list.join(' ')}; `;
  }
  meta.content = policyStr;
  document.head.appendChild(meta);
}

function main() {
  // Unregister service workers
  if ('serviceWorker' in navigator) {
    navigator.serviceWorker.getRegistrations().then((registrations) => {
      for (const registration of registrations) {
        registration.unregister();
      }
    });
  }
  setupContentSecurityPolicy();
  // Settings will be lazy-loaded by the UI components that require them.

  // Load the css. The load is asynchronous and the CSS is not ready by the time
  // appendChild returns.
  const root = getRoot();
  const cssLoadPromise = defer<void>();
  const css = document.createElement('link');
  css.rel = 'stylesheet';
  css.href = root + 'perfetto.css';
  css.onload = () => cssLoadPromise.resolve();
  css.onerror = (err) => cssLoadPromise.reject(err);
  const favicon = document.head.querySelector('#favicon');
  if (favicon instanceof HTMLLinkElement) {
    favicon.href = root + 'assets/favicon.png';
  }

  document.head.append(css);

  // Add Error handlers for JS error and for uncaught exceptions in promises.
  addErrorHandler((err: ErrorDetails) => console.log(err.message, err.stack));
  window.addEventListener('error', (e) => reportError(e));
  window.addEventListener('unhandledrejection', (e) => reportError(e));

  // Prevent pinch zoom.
  document.body.addEventListener(
    'wheel',
    (e: MouseEvent) => {
      if (e.ctrlKey) e.preventDefault();
    },
    {passive: false},
  );

  cssLoadPromise.then(() => onCssLoaded());
}

class BigTraceApp implements m.ClassComponent {
  private sidebarVisible = true;

  oninit() {
    bigTraceSettingsStorage.loadSettings();
  }

  view(vnode: m.Vnode) {
    const currentRoute = m.route.get();

    const items: SidebarMenuItem[] = [
      {
        section: 'home',
        text: 'Home',
        href: '#!/',
        icon: 'home',
        active: currentRoute === '/' || currentRoute === '',
        onclick: () => {},
      },
      {
        section: 'query',
        text: 'Query Editor',
        href: '#!/query',
        icon: 'line_style',
        active: currentRoute === '/query',
        onclick: () => {},
      },
      {
        section: 'settings',
        text: 'Settings',
        href: '#!/settings',
        icon: 'settings',
        active: currentRoute === '/settings',
        onclick: () => {},
      },
    ];

    const currentItem = items.find((item) => item.active);
    const title = currentItem
      ? `${SIDEBAR_SECTIONS[currentItem.section].title} > ${currentItem.text}`
      : '';

    return m(
      '.pf-ui-main',
      {
        style: {
          display: 'flex',
          height: '100vh',
          overflow: 'hidden',
        },
      },
      [
        // Left Sidebar
        m(Sidebar, {
          items,
          onToggleSidebar: () => {
            this.sidebarVisible = !this.sidebarVisible;
          },
          visible: this.sidebarVisible,
        }),

        m(
          '.pf-main-content',
          {
            style: {
              display: 'flex',
              flexDirection: 'column',
              flex: 1,
              overflow: 'hidden',
            },
          },
          [
            m(Topbar, {
              sidebarVisible: this.sidebarVisible,
              onToggleSidebar: () => {
                this.sidebarVisible = !this.sidebarVisible;
              },
              title: title,
            }),
            vnode.children,
          ],
        ),
      ],
    );
  }
}

const BigTraceLayout: m.Component = {
  view(vnode) {
    const theme = settingsStorage.get('theme');
    const themeValue = theme ? theme.get() : 'light';
    return m(ThemeProvider, {theme: themeValue as 'dark' | 'light'}, [
      m(OverlayContainer, {fillHeight: true}, [m(BigTraceApp, vnode.children)]),
    ]);
  },
};

function onCssLoaded() {
  // Clear all the contents of the initial page
  document.body.innerHTML = '';

  m.route.prefix = '#!';
  m.route(document.body, '/', {
    '/': {
      render: () => m(BigTraceLayout, m(HomePage)),
    },
    '/query': {
      onmatch: () => {
        const initialQuery = queryState.initialQuery;
        queryState.initialQuery = undefined;
        return {
          view: () =>
            m(
              BigTraceLayout,
              m(QueryPage, {
                useBrushBackend: true,
                initialQuery,
              }),
            ),
        };
      },
    },
    '/settings': {
      render: () => m(BigTraceLayout, m(SettingsPage)),
    },
  });

  initLiveReload();
}

main();
