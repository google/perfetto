// Copyright (C) 2018 The Android Open Source Project
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
import z from 'zod';
import '../base/disposable_polyfill';
import '../base/static_initializers';
import NON_CORE_PLUGINS from '../gen/all_plugins';
import CORE_PLUGINS from '../gen/all_core_plugins';
import m from 'mithril';
import {defer} from '../base/deferred';
import {addErrorHandler, reportError} from '../base/logging';
import {featureFlags} from '../core/feature_flags';
import {initLiveReload} from '../core/live_reload';
import {raf} from '../core/raf_scheduler';
import {warmupWasmWorker} from '../trace_processor/wasm_engine_proxy';
import {UiMain} from './ui_main';
import {registerDebugGlobals} from './debug';
import {maybeShowErrorDialog} from './error_dialog';
import {installFileDropHandler} from './file_drop_handler';
import {tryLoadIsInternalUserScript} from './is_internal_user_script_loader';
import {HomePage} from './home_page';
import {postMessageHandler} from './post_message_handler';
import {Route, Router} from '../core/router';
import {checkHttpRpcConnection} from './rpc_http_dialog';
import {maybeOpenTraceFromRoute} from './trace_url_handler';
import {HttpRpcEngine} from '../trace_processor/http_rpc_engine';
import {showModal} from '../widgets/modal';
import {IdleDetector} from './idle_detector';
import {IdleDetectorWindow} from './idle_detector_interface';
import {AppImpl} from '../core/app_impl';
import {addLegacyTableTab} from '../components/details/sql_table_tab';
import {configureExtensions} from '../components/extensions';
import {
  addDebugCounterTrack,
  addDebugSliceTrack,
} from '../components/tracks/debug_tracks';
import {addVisualizedArgTracks} from '../components/tracks/visualized_args_tracks';
import {assetSrc, initAssets} from '../base/assets';
import {
  PERFETTO_SETTINGS_STORAGE_KEY,
  SettingsManagerImpl,
} from '../core/settings_manager';
import {LocalStorage} from '../core/local_storage';
import {DurationPrecision, TimestampFormat} from '../public/timeline';
import {timezoneOffsetMap} from '../base/time';
import {ThemeProvider} from './theme_provider';
import {OverlayContainer} from '../widgets/overlay_container';
import {JsonSettingsEditor} from '../components/json_settings_editor';
import {
  CommandInvocation,
  commandInvocationArraySchema,
} from '../core/command_manager';
import {HotkeyConfig, HotkeyContext} from '../widgets/hotkey_context';
import {sleepMs} from '../base/utils';

// =============================================================================
// UI INITIALIZATION STAGES
// =============================================================================
//
// This file orchestrates the Perfetto UI startup through three main stages:
//
//   Time ───────────────────────────────────────────────────────────────────>
//
//   [Module Load]
//        │
//        ├─► main() ───────────────────────────────────────────────────────┐
//        │    ├─ Setup CSP                                                 │
//        │    ├─ Init settings & app                                       │
//        │    ├─ Start CSS load (async) ───────┐                           │
//        │    ├─ Setup error handlers          │                           │
//        │    └─ Register window.onload ───────┼──────────┐                │
//        │                                     │          │                │
//        │    [User sees blank/loading page]   │          │                │
//        │                                     ↓          │                │
//        │                                 CSS loaded     |                │
//        │                                     │          │                │
//        │                        onCssLoaded() ◄──────┘  │                │
//        │                          ├─ Mount Mithril UI   │                │
//        │                          ├─ Register routes    │                │
//        │                          ├─ Init plugins       │                │
//        │                          └─ Check RPC          │                │
//        │                                                │                │
//        │    [User sees interactive UI]                  │                │
//        │                                                ↓                │
//        │                          All resources loaded (fonts, images)   │
//        │                                                │                │
//        │                        onWindowLoaded() ◄──────┘                │
//        │                          ├─ Warmup Wasm (engine_bundle.js)      │
//        │                          └─ Install service worker              │
//        │                                                                 │
//        └─────────────────────────────────────────────────────────────────┘
//
// =============================================================================

const CSP_WS_PERMISSIVE_PORT = featureFlags.register({
  id: 'cspAllowAnyWebsocketPort',
  name: 'Relax Content Security Policy for 127.0.0.1:*',
  description:
    'Allows simultaneous usage of several trace_processor_shell ' +
    '-D --http-port 1234 by opening ' +
    'https://ui.perfetto.dev/#!/?rpc_port=1234',
  defaultValue: false,
});

function routeChange(route: Route) {
  raf.scheduleFullRedraw(() => {
    if (route.fragment) {
      // This needs to happen after the next redraw call. It's not enough
      // to use setTimeout(..., 0); since that may occur before the
      // redraw scheduled above.
      const e = document.getElementById(route.fragment);
      if (e) {
        e.scrollIntoView();
      }
    }
  });
  maybeOpenTraceFromRoute(route);

  // What we want to do here is to check if the hash route has actually changed
  // and if it has then schedule a load of that trace.

  // The only problem is that we might be already loading this trace right now
  // if the user changes pages mid-load, we just haven't loaded the uuid into
  // the URL bar yet.

  // So we need to be very careful to avoid queueing up another load of the same
  // trace which will start immediately after the first one finishes and cause a
  // reload of the same trace.

  // We could just flat out refuse to load a trace while another one is loading,
  // but that isn't very freiendly.

  // Ideally, whenever the uuid in the hash changes, we just chuck away the previously loading trace (whatever it was) and start loading a new one immeidately. Then, when the trace is loaded enough to know its uuid, we put that in the url.

  // if there was no url in the bar last time then we'll reload the trace, that's the only problem.

  // The only thing I can thnink of is to reserve a special cache entry in the url bar for the pending trace. So when we want to load a trace we put the trace content (trace source) in a special reserved 'cache' and put local_cache_key=pending in the URL.
  // Then, when the url bar chanegs and this function is called, we look in that special area and react to it by starting to load the trace.
  // Then when the trace has loaded enough to find out the uuid, we put that in the url bar and clear the pending cache entry. We see that change, but this code will chech against the current UUID of the current trace, see it's the same and ignore the change.
  // What happens if we start loading multiple pending traces? We can just see the fact that there is already a pending trace and refuse to start loading another one until the first one has finished loading.
}

function setupContentSecurityPolicy() {
  // Note: self and sha-xxx must be quoted, urls data: and blob: must not.

  let rpcPolicy = [
    'http://127.0.0.1:9001', // For trace_processor_shell --httpd.
    'ws://127.0.0.1:9001', // Ditto, for the websocket RPC.
    'ws://127.0.0.1:9167', // For Web Device Proxy.
  ];
  if (CSP_WS_PERMISSIVE_PORT.get()) {
    const route = Router.parseUrl(window.location.href);
    if (/^\d+$/.exec(route.args.rpc_port ?? '')) {
      rpcPolicy = [
        `http://127.0.0.1:${route.args.rpc_port}`,
        `ws://127.0.0.1:${route.args.rpc_port}`,
      ];
    }
  }
  const policy = {
    'default-src': [
      `'self'`,
      // Google Tag Manager bootstrap.
      `'sha256-LirUKeorCU4uRNtNzr8tlB11uy8rzrdmqHCX38JSwHY='`,
    ],
    'script-src': [
      `'self'`,
      // TODO(b/201596551): this is required for Wasm after crrev.com/c/3179051
      // and should be replaced with 'wasm-unsafe-eval'.
      `'unsafe-eval'`,
      'https://*.google.com',
      'https://*.googleusercontent.com',
      'https://www.googletagmanager.com',
      'https://*.google-analytics.com',
    ],
    'object-src': ['none'],
    'connect-src': [
      `'self'`,
      'ws://127.0.0.1:8037', // For the adb websocket server.
      'https:', // Allow any HTTPS; service worker firewall adds granular filtering.
      'blob:',
      'data:',
    ].concat(rpcPolicy),
    'img-src': [
      `'self'`,
      'data:',
      'blob:',
      'https://*.google-analytics.com',
      'https://www.googletagmanager.com',
      'https://*.googleapis.com',
    ],
    'style-src': [`'self'`, `'unsafe-inline'`],
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
  // Setup content security policy before anything else.
  setupContentSecurityPolicy();
  initAssets();

  initAppObject();

  // Secure a reference to the app instance.
  const app = AppImpl.instance;

  // Load the css. The load is asynchronous and the CSS is not ready by the time
  // appendChild returns.
  const cssLoadPromise = loadCss();

  // Fonts can take a long time to load, and we want to avoid showing the UI
  // with fallback fonts. To avoid that, we add a 'pf-fonts-loading' class to
  // the body until either the fonts are ready or 15s have passed (whichever
  // happens first).
  setFontsLoadingClass();

  // Load the script to detect if this is a Googler (see comments on globals.ts).
  // This registers macros, SQL packages, and proto descriptors.
  tryLoadIsInternalUserScript(app);

  // Route errors to both the UI bugreport dialog and Analytics (if enabled).
  addErrorHandler(maybeShowErrorDialog);
  addErrorHandler((e) => app.analytics.logError(e));

  // Add Error handlers for JS error and for uncaught exceptions in promises.
  window.addEventListener('error', (e) => reportError(e));
  window.addEventListener('unhandledrejection', (e) => reportError(e));

  // Put debug variables in the global scope for better debugging.
  registerDebugGlobals();

  // Prevent pinch zoom.
  document.body.addEventListener(
    'wheel',
    (e: MouseEvent) => {
      if (e.ctrlKey) e.preventDefault();
    },
    {passive: false},
  );

  cssLoadPromise.then(() => onCssLoaded(app));

  (window as {} as IdleDetectorWindow).waitForPerfettoIdle = (ms?: number) => {
    return new IdleDetector().waitForPerfettoIdle(ms);
  };

  // Keep at the end. Potentially it calls into the next stage (onWindowLoaded).
  if (document.readyState === 'complete') {
    onWindowLoaded(app);
  } else {
    window.addEventListener('load', () => onWindowLoaded(app));
  }
}

function initAppObject() {
  const settingsManager = new SettingsManagerImpl(
    new LocalStorage(PERFETTO_SETTINGS_STORAGE_KEY),
  );

  // Initialize core settings...
  const timestampFormatSetting = settingsManager.register({
    id: 'timestampFormat',
    name: 'Timestamp format',
    description: 'The format of timestamps throughout Perfetto.',
    schema: z.enum(TimestampFormat),
    defaultValue: TimestampFormat.Timecode,
  });

  const timezoneOverrideSetting = settingsManager.register({
    id: 'timezoneOverride',
    name: 'Timezone Override',
    description:
      "When 'Timestamp Format' is set to 'CustomTimezone', this setting controls which timezone is used.",
    schema: z.enum(Object.keys(timezoneOffsetMap) as [string, ...string[]]),
    defaultValue: '(UTC+00:00) London, Dublin, Lisbon, Casablanca', // UTC by default.
  });

  const durationPrecisionSetting = settingsManager.register({
    id: 'durationPrecision',
    name: 'Duration precision',
    description: 'The precision of durations throughout Perfetto.',
    schema: z.enum(DurationPrecision),
    defaultValue: DurationPrecision.Full,
  });

  const analyticsSetting = settingsManager.register({
    id: 'analyticsEnable',
    name: 'Enable UI telemetry',
    description: `
      This setting controls whether the Perfetto UI logs coarse-grained
      information about your usage of the UI and any errors encountered. This
      information helps us understand how the UI is being used and allows us to
      better prioritise features and fix bugs. If this option is disabled,
      no information will be logged.

      Note: even if this option is enabled, information about the *contents* of
      traces is *not* logged.

      Note: this setting only has an effect on the ui.perfetto.dev and localhost
      origins: all other origins do not log telemetry even if this option is
      enabled.
    `,
    schema: z.boolean(),
    defaultValue: true,
    requiresReload: true,
  });

  const startupCommandsEditor = new JsonSettingsEditor<CommandInvocation[]>({
    schema: commandInvocationArraySchema,
  });

  const startupCommandsSetting = settingsManager.register({
    id: 'startupCommands',
    name: 'Startup Commands',
    description: `
      Commands to run automatically after a trace loads and any saved state is
      restored. These commands execute as if a user manually invoked them after
      the trace is fully ready, making them ideal for automating common
      post-load actions like running queries, expanding tracks, or setting up
      custom views.
    `,
    schema: commandInvocationArraySchema,
    defaultValue: [],
    render: (setting) => startupCommandsEditor.render(setting),
  });

  const enforceStartupCommandAllowlistSetting = settingsManager.register({
    id: 'enforceStartupCommandAllowlist',
    name: 'Enforce Startup Command Allowlist',
    description: `
      When enabled, only commands in the predefined allowlist can be executed
      as startup commands. When disabled, all startup commands will be
      executed without filtering.

      The command allowlist encodes the set of commands which Perfetto UI
      maintainers expect to maintain backwards compatibility for the forseeable\
      future.

      WARNING: if this setting is disabled, any command outside the allowlist
      has *no* backwards compatibility guarantees and is can change without
      warning at any time.
    `,
    schema: z.boolean(),
    defaultValue: true,
  });

  AppImpl.initialize({
    initialRouteArgs: Router.parseUrl(window.location.href).args,
    settingsManager,
    timestampFormatSetting,
    durationPrecisionSetting,
    timezoneOverrideSetting,
    analyticsSetting,
    startupCommandsSetting,
    enforceStartupCommandAllowlistSetting,
  });
}

function loadCss() {
  const cssLoadPromise = defer<void>();
  const css = document.createElement('link');
  css.rel = 'stylesheet';
  css.href = assetSrc('perfetto.css');
  css.onload = () => cssLoadPromise.resolve();
  css.onerror = (err) => cssLoadPromise.reject(err);
  const favicon = document.head.querySelector('#favicon');
  if (favicon instanceof HTMLLinkElement) {
    favicon.href = assetSrc('assets/favicon.png');
  }
  document.head.append(css);
  return cssLoadPromise;
}

function setFontsLoadingClass() {
  document.body.classList.add('pf-fonts-loading');
  Promise.race([document.fonts.ready, sleepMs(15000)]).then(() => {
    document.body.classList.remove('pf-fonts-loading');
  });
}

function onCssLoaded(app: AppImpl) {
  // Clear all the contents of the initial page (e.g. the <pre> error message)
  // And replace it with the root <main> element which will be used by mithril.
  document.body.innerHTML = '';

  const pages = app.pages;
  pages.registerPage({route: '/', render: () => m(HomePage)});
  const router = new Router();
  router.onRouteChanged = routeChange;

  const themeSetting = app.settings.register({
    id: 'theme',
    name: 'UI Theme',
    description: 'Changes the color palette used throughout the UI.',
    schema: z.enum(['dark', 'light']),
    defaultValue: 'light',
  } as const);

  // Add command to toggle the theme.
  app.commands.registerCommand({
    id: 'dev.perfetto.ToggleTheme',
    name: 'Toggle UI Theme (Dark/Light)',
    callback: () => {
      const currentTheme = themeSetting.get();
      themeSetting.set(currentTheme === 'dark' ? 'light' : 'dark');
    },
  });

  // Mount the main mithril component. This also forces a sync render pass.
  raf.mount(document.body, {
    view: () => {
      const commands = app.commands;
      const hotkeys: HotkeyConfig[] = [];
      for (const {id, defaultHotkey} of commands.commands) {
        if (defaultHotkey) {
          hotkeys.push({
            callback: () => commands.runCommand(id),
            hotkey: defaultHotkey,
          });
        }
      }

      // Add a dummy binding to prevent Mod+P from opening the print dialog.
      // Firstly, there is no reason to print the UI. Secondly, plugins might
      // register a Mod+P hotkey later at trace load time. It would be confusing
      // if this hotkey sometimes does what you want, but sometimes shows the
      // print dialog.
      hotkeys.push({
        hotkey: 'Mod+P',
        callback: () => {},
      });

      const currentTraceId = app.trace?.engine.engineId ?? 'no-trace';

      // Trace data is cached inside many components on the tree. To avoid
      // issues with stale data when reloading a trace, we force-remount the
      // entire tree whenever the trace changes by using the trace ID as part of
      // the key. We also know that UIMain reloads the theme CSS variables on
      // mount, so include the theme in the key so that changing the theme also
      // forces a remount.
      const uiMainKey = `${currentTraceId}-${themeSetting.get()}`;

      return m(ThemeProvider, {theme: themeSetting.get()}, [
        m(
          HotkeyContext,
          {
            hotkeys,
            fillHeight: true,
            // When embedded, hotkeys should be scoped to the context element to
            // avoid interfering with the parent page. In standalone mode,
            // document-level binding provides better UX (e.g., PGUP/PGDN scroll
            // behavior).
            focusable: false,
          },
          m(OverlayContainer, {fillHeight: true}, m(UiMain, {key: uiMainKey})),
        ),
      ]);
    },
  });

  if (
    (location.origin.startsWith('http://localhost:') ||
      location.origin.startsWith('http://127.0.0.1:')) &&
    !app.embeddedMode &&
    !app.testingMode
  ) {
    initLiveReload();
  }

  // Will update the chip on the sidebar footer that notifies that the RPC is
  // connected. Has no effect on the controller (which will repeat this check
  // before creating a new engine).
  // Don't auto-open any trace URLs until we get a response here because we may
  // accidentially clober the state of an open trace processor instance
  // otherwise.
  maybeChangeRpcPortFromFragment();
  checkHttpRpcConnection().then(() => {
    const route = Router.parseUrl(window.location.href);
    if (!app.embeddedMode) {
      installFileDropHandler();
    }

    // Don't allow postMessage or opening trace from route when the user says
    // that they want to reuse the already loaded trace in trace processor.
    const traceSource = app.trace?.traceInfo.source;
    if (traceSource && traceSource.type === 'HTTP_RPC') {
      return;
    }

    // Add support for opening traces from postMessage().
    window.addEventListener('message', postMessageHandler, {passive: true});

    // Handles the initial ?local_cache_key=123 or ?s=permalink or ?url=...
    // cases.
    routeChange(route);
  });

  // Initialize plugins, now that we are ready to go.
  const pluginManager = app.plugins;
  CORE_PLUGINS.forEach((p) => pluginManager.registerPlugin(p, true));
  NON_CORE_PLUGINS.forEach((p) => pluginManager.registerPlugin(p, false));
  const route = Router.parseUrl(window.location.href);
  const overrides = (route.args.enablePlugins ?? '').split(',');
  pluginManager.activatePlugins(app, overrides);

  // Initialize analytics after plugins have been activated, so that plugins
  // (e.g. ExtensionServers) can add dimensions before GA is configured.
  app.analytics.initialize();
}

// This function is called only later after all the sub-resources (fonts,
// images) have been loaded.
function onWindowLoaded(app: AppImpl) {
  // These two functions cause large network fetches and are not load bearing.
  app.serviceWorkerController.install();
  warmupWasmWorker();
}

// If the URL is /#!?rpc_port=1234, change the default RPC port.
// For security reasons, this requires toggling a flag. Detect this and tell the
// user what to do in this case.
function maybeChangeRpcPortFromFragment() {
  const route = Router.parseUrl(window.location.href);
  if (route.args.rpc_port !== undefined) {
    if (!CSP_WS_PERMISSIVE_PORT.get()) {
      showModal({
        title: 'Using a different port requires a flag change',
        content: m(
          'div',
          m(
            'span',
            'For security reasons before connecting to a non-standard ' +
              'TraceProcessor port you need to manually enable the flag to ' +
              'relax the Content Security Policy and restart the UI.',
          ),
        ),
        buttons: [
          {
            text: 'Take me to the flags page',
            primary: true,
            action: () => Router.navigate('#!/flags/cspAllowAnyWebsocketPort'),
          },
        ],
      });
    } else {
      HttpRpcEngine.rpcPort = route.args.rpc_port;
    }
  }
}

// TODO(primiano): this injection is to break a cirular dependency. See
// comment in sql_table_tab_interface.ts. Remove once we add an extension
// point for context menus.
configureExtensions({
  addDebugCounterTrack,
  addDebugSliceTrack,
  addVisualizedArgTracks,
  addLegacySqlTableTab: addLegacyTableTab,
});

main();
