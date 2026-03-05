// Copyright (C) 2023 The Android Open Source Project
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
import {raf} from '../core/raf_scheduler';
import {ThemeProvider} from '../frontend/theme_provider';
import {OverlayContainer} from '../widgets/overlay_container';
import {D3ChartsPage} from '../plugins/dev.perfetto.D3ChartsPage/d3_charts_page';
import {Button} from '../widgets/button';
import {Icon} from '../widgets/icon';

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
    'connect-src': [`'self'`, 'https://brush-googleapis.corp.google.com'],
    'img-src': [`'self'`, 'data:', 'blob:'],
    'style-src': [
      `'self'`,
      `'sha256-47DEQpj8HBSa+/TImW+5JCeuQeRkm5NMpJWZG3hSuFU='`,
      `'sha256-yRQRG6LLKMvjvigtzXD1f8VRZSYY7J8fM2ZLfdMaHKg='`,
    ],
    'navigate-to': ['https://*.perfetto.dev', 'self'],
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
  setupContentSecurityPolicy();

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
  private currentPage = 'bigtrace';

  view() {
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
        // Left Sidebar (only render when visible)
        this.sidebarVisible &&
          m(
            'nav.pf-sidebar',
            {
              style: {
                width: '250px',
                flexShrink: 0,
              },
            },
            [
              m(
                'header',
                {
                  style: {
                    padding: '16px',
                    borderBottom: '1px solid var(--pf-color-border)',
                    display: 'flex',
                    alignItems: 'center',
                    justifyContent: 'space-between',
                  },
                },
                [
                  m('h1', {style: {margin: 0, fontSize: '18px'}}, 'BigTrace'),
                  m(Button, {
                    icon: 'menu',
                    onclick: () => {
                      this.sidebarVisible = !this.sidebarVisible;
                    },
                  }),
                ],
              ),
              m(
                '.pf-sidebar__scroll',
                m('.pf-sidebar__scroll-container', [
                  // BigTrace Section
                  m('section.pf-sidebar__section--expanded', [
                    m(
                      '.pf-sidebar__section-header',
                      m('h1', {title: 'BigTrace'}, 'BigTrace'),
                    ),
                    m('.pf-sidebar__section-content', [
                      m('ul', [
                        m(
                          'li',
                          m(
                            'a',
                            {
                              class:
                                this.currentPage === 'bigtrace' ? 'active' : '',
                              onclick: () => {
                                this.currentPage = 'bigtrace';
                              },
                              href: '#',
                            },
                            [m(Icon, {icon: 'line_style'}), 'Query Editor'],
                          ),
                        ),
                      ]),
                    ]),
                  ]),
                ]),
              ),
            ],
          ),

        // Main content - D3ChartsPage
        m(
          '.bigtrace-content',
          {
            style: {
              flex: 1,
              overflow: 'hidden',
            },
          },
          m(D3ChartsPage, {
            useBrushBackend: true,
            initialQuery: undefined,
            hideSqlEditor: false,
            sidebarVisible: this.sidebarVisible,
            onToggleSidebar: () => {
              this.sidebarVisible = !this.sidebarVisible;
            },
          }),
        ),
      ],
    );
  }
}

function onCssLoaded() {
  // Clear all the contents of the initial page
  document.body.innerHTML = '';

  raf.mount(document.body, {
    view: () =>
      m(ThemeProvider, {theme: 'light'}, [
        m(OverlayContainer, {fillHeight: true}, [m(BigTraceApp)]),
      ]),
  });

  initLiveReload();
}

main();
