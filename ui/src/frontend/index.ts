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
import {
  DEFAULT_TRACK_MIN_HEIGHT_PX,
  MINIMUM_TRACK_MIN_HEIGHT_PX,
  TRACK_MIN_HEIGHT_SETTING,
} from './timeline_page/track_view';
import {renderTimelinePage} from './timeline_page/timeline_page';
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
import {addQueryResultsTab} from '../components/query_table/query_result_tab';
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
//        │    ├─ Start CSS load (async) ──────┐                            │
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
      'https://*.google-analytics.com',
      'https://*.googleapis.com', // For Google Cloud Storage fetches.
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

  // Create settings Manager
  const settingsManager = new SettingsManagerImpl(
    new LocalStorage(PERFETTO_SETTINGS_STORAGE_KEY),
  );

  // Initialize core settings...
  const timestampFormatSetting = settingsManager.register({
    id: 'timestampFormat',
    name: 'Timestamp format',
    description: 'The format of timestamps throughout Perfetto.',
    schema: z.nativeEnum(TimestampFormat),
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
    schema: z.nativeEnum(DurationPrecision),
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

  // Load the css. The load is asynchronous and the CSS is not ready by the time
  // appendChild returns.
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
  document.body.classList.add('pf-fonts-loading');
  document.head.append(css);

  Promise.race([document.fonts.ready, sleepMs(15000)]).then(() => {
    document.body.classList.remove('pf-fonts-loading');
  });

  // Load the script to detect if this is a Googler (see comments on globals.ts)
  // and initialize GA after that (or after a timeout if something goes wrong).
  const app = AppImpl.instance;
  tryLoadIsInternalUserScript(app).then(() => {
    app.analytics.initialize(app.isInternalUser);
    app.notifyOnExtrasLoadingCompleted();
  });

  // Route errors to both the UI bugreport dialog and Analytics (if enabled).
  addErrorHandler(maybeShowErrorDialog);
  addErrorHandler((e) => AppImpl.instance.analytics.logError(e));

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

  cssLoadPromise.then(() => onCssLoaded());

  (window as {} as IdleDetectorWindow).waitForPerfettoIdle = (ms?: number) => {
    return new IdleDetector().waitForPerfettoIdle(ms);
  };

  // Keep at the end. Potentially it calls into the next stage (onWindowLoaded).
  if (document.readyState === 'complete') {
    onWindowLoaded();
  } else {
    window.addEventListener('load', () => onWindowLoaded());
  }
}

function onCssLoaded() {
  // Clear all the contents of the initial page (e.g. the <pre> error message)
  // And replace it with the root <main> element which will be used by mithril.
  document.body.innerHTML = '';

  const pages = AppImpl.instance.pages;
  pages.registerPage({route: '/', render: () => m(HomePage)});
  pages.registerPage({route: '/viewer', render: () => renderTimelinePage()});
  const router = new Router();
  router.onRouteChanged = routeChange;

  const themeSetting = AppImpl.instance.settings.register({
    id: 'theme',
    name: 'UI Theme',
    description: 'Changes the color palette used throughout the UI.',
    schema: z.enum(['dark', 'light']),
    defaultValue: 'light',
  } as const);

  AppImpl.instance.settings.register({
    id: TRACK_MIN_HEIGHT_SETTING,
    name: 'Track Height',
    description:
      'Minimum height of tracks in the trace viewer page, in pixels.',
    schema: z.number().int().min(MINIMUM_TRACK_MIN_HEIGHT_PX),
    defaultValue: DEFAULT_TRACK_MIN_HEIGHT_PX,
  });

  // Add command to toggle the theme.
  AppImpl.instance.commands.registerCommand({
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
      const app = AppImpl.instance;
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
    !AppImpl.instance.embeddedMode &&
    !AppImpl.instance.testingMode
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
    if (!AppImpl.instance.embeddedMode) {
      installFileDropHandler();
    }

    // Don't allow postMessage or opening trace from route when the user says
    // that they want to reuse the already loaded trace in trace processor.
    const traceSource = AppImpl.instance.trace?.traceInfo.source;
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
  const pluginManager = AppImpl.instance.plugins;
  CORE_PLUGINS.forEach((p) => pluginManager.registerPlugin(p, true));
  NON_CORE_PLUGINS.forEach((p) => pluginManager.registerPlugin(p, false));
  const route = Router.parseUrl(window.location.href);
  const overrides = (route.args.enablePlugins ?? '').split(',');
  pluginManager.activatePlugins(AppImpl.instance, overrides);
}

// This function is called only later after all the sub-resources (fonts,
// images) have been loaded.
function onWindowLoaded() {
  // These two functions cause large network fetches and are not load bearing.
  AppImpl.instance.serviceWorkerController.install();
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
  addQueryResultsTab,
});

main();
