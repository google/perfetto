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
import '../base/disposable_polyfill';
import '../base/static_initializers';
import '../gen/all_plugins';
import '../gen/all_core_plugins';

import {Draft} from 'immer';
import m from 'mithril';

import {defer} from '../base/deferred';
import {addErrorHandler, reportError} from '../base/logging';
import {Store} from '../base/store';
import {Actions, DeferredAction, StateActions} from '../common/actions';
import {flattenArgs, traceEvent} from '../common/metatracing';
import {pluginManager} from '../common/plugins';
import {State} from '../common/state';
import {initController, runControllers} from '../controller';
import {isGetCategoriesResponse} from '../controller/chrome_proxy_record_controller';
import {RECORDING_V2_FLAG, featureFlags} from '../core/feature_flags';
import {initLiveReload} from '../core/live_reload';
import {raf} from '../core/raf_scheduler';
import {initWasm} from '../trace_processor/wasm_engine_proxy';
import {setScheduleFullRedraw} from '../widgets/raf';

import {App} from './app';
import {initCssConstants} from './css_constants';
import {registerDebugGlobals} from './debug';
import {maybeShowErrorDialog} from './error_dialog';
import {installFileDropHandler} from './file_drop_handler';
import {FlagsPage} from './flags_page';
import {globals} from './globals';
import {HomePage} from './home_page';
import {InsightsPage} from './insights_page';
import {MetricsPage} from './metrics_page';
import {PluginsPage} from './plugins_page';
import {postMessageHandler} from './post_message_handler';
import {QueryPage} from './query_page';
import {RecordPage, updateAvailableAdbDevices} from './record_page';
import {RecordPageV2} from './record_page_v2';
import {Route, Router} from './router';
import {CheckHttpRpcConnection} from './rpc_http_dialog';
import {TraceInfoPage} from './trace_info_page';
import {maybeOpenTraceFromRoute} from './trace_url_handler';
import {ViewerPage} from './viewer_page';
import {VizPage} from './viz_page';
import {WidgetsPage} from './widgets_page';
import {HttpRpcEngine} from '../trace_processor/http_rpc_engine';
import {showModal} from '../widgets/modal';
import {FilesPage} from './files_page';

const EXTENSION_ID = 'lfmkphfpdbjijhpomgecfikhfohaoine';

const CSP_WS_PERMISSIVE_PORT = featureFlags.register({
  id: 'cspAllowAnyWebsocketPort',
  name: 'Relax Content Security Policy for 127.0.0.1:*',
  description:
    'Allows simultaneous usage of several trace_processor_shell ' +
    '-D --http-port 1234 by opening ' +
    'https://ui.perfetto.dev/#!/?rpc_port=1234',
  defaultValue: false,
});

function setExtensionAvailability(available: boolean) {
  globals.dispatch(
    Actions.setExtensionAvailable({
      available,
    }),
  );
}

function routeChange(route: Route) {
  raf.scheduleFullRedraw();
  maybeOpenTraceFromRoute(route);
  if (route.fragment) {
    // This needs to happen after the next redraw call. It's not enough
    // to use setTimeout(..., 0); since that may occur before the
    // redraw scheduled above.
    raf.addPendingCallback(() => {
      const e = document.getElementById(route.fragment);
      if (e) {
        e.scrollIntoView();
      }
    });
  }
}

function setupContentSecurityPolicy() {
  // Note: self and sha-xxx must be quoted, urls data: and blob: must not.

  let rpcPolicy = [
    'http://127.0.0.1:9001', // For trace_processor_shell --httpd.
    'ws://127.0.0.1:9001', // Ditto, for the websocket RPC.
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
  if (CSP_WS_PERMISSIVE_PORT.get()) {
    const route = Router.parseUrl(window.location.href);
    if (route.args.rpc_host_port !== undefined) {
      rpcPolicy = [
        `http://${route.args.rpc_host_port}`,
        `ws://${route.args.rpc_host_port}`,
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
      'ws://'+ window.location.hostname +':9001',
      'https://*.google-analytics.com',
      'https://*.googleapis.com', // For Google Cloud Storage fetches.
      'http://'+ window.location.hostname +':9001',
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
  // Wire up raf for widgets.
  setScheduleFullRedraw(() => raf.scheduleFullRedraw());

  setupContentSecurityPolicy();

  // Load the css. The load is asynchronous and the CSS is not ready by the time
  // appendChild returns.
  const cssLoadPromise = defer<void>();
  const css = document.createElement('link');
  css.rel = 'stylesheet';
  css.href = globals.root + 'perfetto.css';
  css.onload = () => cssLoadPromise.resolve();
  css.onerror = (err) => cssLoadPromise.reject(err);
  const favicon = document.head.querySelector('#favicon');
  if (favicon instanceof HTMLLinkElement) {
    favicon.href = globals.root + 'assets/favicon.png';
  }

  // Load the script to detect if this is a Googler (see comments on globals.ts)
  // and initialize GA after that (or after a timeout if something goes wrong).
  const script = document.createElement('script');
  script.src =
    'https://storage.cloud.google.com/perfetto-ui-internal/is_internal_user.js';
  script.async = true;
  script.onerror = () => globals.logging.initialize();
  script.onload = () => globals.logging.initialize();
  setTimeout(() => globals.logging.initialize(), 5000);

  document.head.append(script, css);

  // Route errors to both the UI bugreport dialog and Analytics (if enabled).
  addErrorHandler(maybeShowErrorDialog);
  addErrorHandler((e) => globals.logging.logError(e));

  // Add Error handlers for JS error and for uncaught exceptions in promises.
  window.addEventListener('error', (e) => reportError(e));
  window.addEventListener('unhandledrejection', (e) => reportError(e));

  const extensionLocalChannel = new MessageChannel();

  initWasm(globals.root);
  initController(extensionLocalChannel.port1);

  // These need to be set before globals.initialize.
  const route = Router.parseUrl(window.location.href);
  globals.embeddedMode = route.args.mode === 'embedded';
  globals.hideSidebar = route.args.hideSidebar === true;

  globals.initialize(stateActionDispatcher);

  globals.serviceWorkerController.install();

  globals.store.subscribe(scheduleRafAndRunControllersOnStateChange);
  globals.publishRedraw = () => raf.scheduleFullRedraw();

  // We proxy messages between the extension and the controller because the
  // controller's worker can't access chrome.runtime.
  const extensionPort =
    // eslint-disable-next-line @typescript-eslint/strict-boolean-expressions
    window.chrome && chrome.runtime
      ? chrome.runtime.connect(EXTENSION_ID)
      : undefined;

  setExtensionAvailability(extensionPort !== undefined);

  if (extensionPort) {
    extensionPort.onDisconnect.addListener((_) => {
      setExtensionAvailability(false);
      void chrome.runtime.lastError; // Needed to not receive an error log.
    });
    // This forwards the messages from the extension to the controller.
    extensionPort.onMessage.addListener(
      (message: object, _port: chrome.runtime.Port) => {
        if (isGetCategoriesResponse(message)) {
          globals.dispatch(Actions.setChromeCategories(message));
          return;
        }
        extensionLocalChannel.port2.postMessage(message);
      },
    );
  }

  // This forwards the messages from the controller to the extension
  extensionLocalChannel.port2.onmessage = ({data}) => {
    if (extensionPort) extensionPort.postMessage(data);
  };

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

  if (globals.testing) {
    document.body.classList.add('testing');
  }
}

function onCssLoaded() {
  initCssConstants();
  // Clear all the contents of the initial page (e.g. the <pre> error message)
  // And replace it with the root <main> element which will be used by mithril.
  document.body.innerHTML = '';

  const router = new Router({
    '/': HomePage,
    '/viewer': ViewerPage,
    '/record': RECORDING_V2_FLAG.get() ? RecordPageV2 : RecordPage,
    '/query': QueryPage,
    '/insights': InsightsPage,
    '/flags': FlagsPage,
    '/metrics': MetricsPage,
    '/info': TraceInfoPage,
    '/widgets': WidgetsPage,
    '/viz': VizPage,
    '/plugins': PluginsPage,
    '/files': FilesPage,
  });
  router.onRouteChanged = routeChange;

  raf.domRedraw = () => {
    m.render(document.body, m(App, router.resolve()));
  };

  if (
    (location.origin.startsWith('http://localhost:') ||
      location.origin.startsWith('http://127.0.0.1:')) &&
    !globals.embeddedMode &&
    !globals.testing
  ) {
    initLiveReload();
  }

  if (!RECORDING_V2_FLAG.get()) {
    updateAvailableAdbDevices();
    try {
      navigator.usb.addEventListener('connect', () =>
        updateAvailableAdbDevices(),
      );
      navigator.usb.addEventListener('disconnect', () =>
        updateAvailableAdbDevices(),
      );
    } catch (e) {
      console.error('WebUSB API not supported');
    }
  }

  // Will update the chip on the sidebar footer that notifies that the RPC is
  // connected. Has no effect on the controller (which will repeat this check
  // before creating a new engine).
  // Don't auto-open any trace URLs until we get a response here because we may
  // accidentially clober the state of an open trace processor instance
  // otherwise.
  maybeChangeRpcPortFromFragment();
  CheckHttpRpcConnection().then(() => {
    const route = Router.parseUrl(window.location.href);
    globals.dispatch(
      Actions.maybeSetPendingDeeplink({
        ts: route.args.ts,
        tid: route.args.tid,
        dur: route.args.dur,
        pid: route.args.pid,
        query: route.args.query,
        visStart: route.args.visStart,
        visEnd: route.args.visEnd,
      }),
    );

    if (!globals.embeddedMode) {
      installFileDropHandler();
    }

    // Don't allow postMessage or opening trace from route when the user says
    // that they want to reuse the already loaded trace in trace processor.
    const engine = globals.getCurrentEngine();
    if (engine && engine.source.type === 'HTTP_RPC') {
      return;
    }

    // Add support for opening traces from postMessage().
    window.addEventListener('message', postMessageHandler, {passive: true});

    // Handles the initial ?local_cache_key=123 or ?s=permalink or ?url=...
    // cases.
    routeChange(route);
  });

  // Force one initial render to get everything in place
  m.render(document.body, m(App, router.resolve()));

  // Initialize plugins, now that we are ready to go
  pluginManager.initialize();
}

// If the URL is /#!?rpc_port=1234, change the default RPC port.
// For security reasons, this requires toggling a flag. Detect this and tell the
// user what to do in this case.
function maybeChangeRpcPortFromFragment() {
  const route = Router.parseUrl(window.location.href);
  if (route.args.rpc_port !== undefined || route.args.rpc_host_port !== undefined) {
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
      if (route.args.rpc_port !== undefined ) {
        HttpRpcEngine.rpcHostPort = `127.0.0.1:${route.args.rpc_port}`;
      }
      if (route.args.rpc_host_port !== undefined) {
        HttpRpcEngine.rpcHostPort = route.args.rpc_host_port;
      }
    }
  }
}

function stateActionDispatcher(actions: DeferredAction[]) {
  const edits = actions.map((action) => {
    return traceEvent(
      `action.${action.type}`,
      () => {
        return (draft: Draft<State>) => {
          // eslint-disable-next-line @typescript-eslint/no-explicit-any
          (StateActions as any)[action.type](draft, action.args);
        };
      },
      {
        args: flattenArgs(action.args),
      },
    );
  });
  globals.store.edit(edits);
}

function scheduleRafAndRunControllersOnStateChange(
  store: Store<State>,
  oldState: State,
) {
  // Only redraw if something actually changed
  if (oldState !== store.state) {
    raf.scheduleFullRedraw();
  }
  // Run in a separate task to avoid avoid reentry.
  setTimeout(runControllers, 0);
}

main();
