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
import {BigTraceApp as BigTraceAppSingleton} from './bigtrace_app';
import {OmniboxMode} from '../core/omnibox_manager';
import {Sidebar, SidebarMenuItem} from './layout/sidebar';
import {HotkeyConfig, HotkeyContext} from '../widgets/hotkey_context';
import {initAssets} from '../base/assets';
import {getCurrentRoute, initRouter} from './router';
import {Routes} from './routes';

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
  initAssets();
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
  addErrorHandler((err: ErrorDetails) => console.error(err.message, err.stack));
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

// Allows the sidebar toggle command (registered globally) to reach into the
// BigTraceApp component's local state.
let sidebarToggleFn: (() => void) | undefined;

class BigTraceLayout implements m.ClassComponent {
  private sidebarVisible = true;

  oninit() {
    bigTraceSettingsStorage.loadSettings();
    sidebarToggleFn = () => {
      this.sidebarVisible = !this.sidebarVisible;
    };
  }

  view(vnode: m.Vnode) {
    const currentRoute = getCurrentRoute();

    const items: SidebarMenuItem[] = [
      {
        section: 'home',
        text: 'Home',
        href: `#!${Routes.HOME}`,
        icon: 'home',
        active: currentRoute === Routes.HOME || currentRoute === '',
        onclick: () => {},
      },
      {
        section: 'query',
        text: 'Query Editor',
        href: `#!${Routes.QUERY}`,
        icon: 'line_style',
        active: currentRoute === Routes.QUERY,
        onclick: () => {},
      },
      {
        section: 'settings',
        text: 'Settings',
        href: `#!${Routes.SETTINGS}`,
        icon: 'settings',
        active: currentRoute === Routes.SETTINGS,
        onclick: () => {},
      },
    ];

    return m('main.pf-ui-main', [
      m(Sidebar, {
        items,
        onToggleSidebar: () => {
          this.sidebarVisible = !this.sidebarVisible;
        },
        visible: this.sidebarVisible,
      }),
      m(Topbar, {sidebarVisible: this.sidebarVisible}),
      m('.pf-ui-main__page-container', vnode.children),
    ]);
  }
}

// Root component: routing, theme, hotkeys, and layout.
// Uses m.mount (not m.route) so that all rendering goes through the raf
// scheduler's mount system. m.route() caches the original m.mount and
// bypasses the raf scheduler, which breaks cross-tree redraws for
// portal-based popups (e.g. the omnibox dropdown).
class BigTraceRoot implements m.ClassComponent {
  private prevRoute = '';
  private queryInitialQuery: string | undefined;

  view(): m.Children {
    const route = getCurrentRoute();

    // Capture initialQuery on first navigation to /query.
    if (route === Routes.QUERY && this.prevRoute !== Routes.QUERY) {
      this.queryInitialQuery = queryState.initialQuery;
      queryState.initialQuery = undefined;
    }
    this.prevRoute = route;

    const page = this.resolvePage(route);

    const theme = settingsStorage.get('theme');
    const themeValue = theme ? theme.get() : 'light';

    const commands = BigTraceAppSingleton.instance.commands;
    const hotkeys: HotkeyConfig[] = [];
    for (const {id, defaultHotkey} of commands.commands) {
      if (defaultHotkey) {
        hotkeys.push({
          callback: () => commands.runCommand(id),
          hotkey: defaultHotkey,
        });
      }
    }

    return m(ThemeProvider, {theme: themeValue as 'dark' | 'light'}, [
      m(
        HotkeyContext,
        {hotkeys, fillHeight: true, focusable: false},
        m(OverlayContainer, {fillHeight: true}, [m(BigTraceLayout, page)]),
      ),
    ]);
  }

  private resolvePage(route: string): m.Children {
    switch (route) {
      case Routes.QUERY:
        return m(QueryPage, {
          useBrushBackend: true,
          initialQuery: this.queryInitialQuery,
        });
      case Routes.SETTINGS:
        return m(SettingsPage);
      default:
        return m(HomePage);
    }
  }
}

function registerCommands() {
  const app = BigTraceAppSingleton.instance;

  app.commands.registerCommand({
    id: 'bigtrace.ToggleTheme',
    name: 'Toggle UI Theme (Dark/Light)',
    callback: () => {
      const theme = settingsStorage.get('theme');
      if (theme) theme.set(theme.get() === 'light' ? 'dark' : 'light');
    },
  });

  app.commands.registerCommand({
    id: 'bigtrace.OpenCommandPalette',
    name: 'Open command palette',
    callback: () => app.omnibox.setMode(OmniboxMode.Command),
    defaultHotkey: '!Mod+Shift+P',
  });

  app.commands.registerCommand({
    id: 'bigtrace.ToggleLeftSidebar',
    name: 'Toggle left sidebar',
    callback: () => {
      sidebarToggleFn?.();
    },
    defaultHotkey: '!Mod+B',
  });
}

function onCssLoaded() {
  document.body.innerHTML = '';
  initRouter();
  m.mount(document.body, BigTraceRoot);
  initLiveReload();
  registerCommands();
}

main();
