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
import '../core/static_initializers';
import '../gen/all_plugins';

import {Draft} from 'immer';
import m from 'mithril';

import {defer} from '../base/deferred';
import {reportError, setErrorHandler} from '../base/logging';
import {Actions, DeferredAction, StateActions} from '../common/actions';
import {CommandManager} from '../common/commands';
import {createEmptyState} from '../common/empty_state';
import {RECORDING_V2_FLAG} from '../common/feature_flags';
import {flattenArgs, traceEvent} from '../common/metatracing';
import {pluginManager, pluginRegistry} from '../common/plugins';
import {State} from '../common/state';
import {ViewerImpl} from '../common/viewer';
import {initWasm} from '../common/wasm_engine_proxy';
import {initController, runControllers} from '../controller';
import {
  isGetCategoriesResponse,
} from '../controller/chrome_proxy_record_controller';
import {raf} from '../core/raf_scheduler';
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
import {initLiveReloadIfLocalhost} from './live_reload';
import {MetricsPage} from './metrics_page';
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

const EXTENSION_ID = 'lfmkphfpdbjijhpomgecfikhfohaoine';

class FrontendApi {
  constructor() {
    globals.store.subscribe(this.handleStoreUpdate);
  }

  private handleStoreUpdate = (state: State, oldState: State) => {
    // If the visible time in the global state has been updated more
    // recently than the visible time handled by the frontend @ 60fps,
    // update it. This typically happens when restoring the state from a
    // permalink.
    globals.frontendLocalState.mergeState(state.frontendLocalState);

    // Only redraw if something other than the frontendLocalState changed.
    let key: keyof State;
    for (key in state) {
      if (key !== 'frontendLocalState' && key !== 'visibleTracks' &&
          oldState[key] !== state[key]) {
        raf.scheduleFullRedraw();
        break;
      }
    }

    // Run in microtask to aboid avoid reentry
    setTimeout(runControllers, 0);
  };

  dispatchMultiple(actions: DeferredAction[]) {
    const edits = actions.map((action) => {
      return traceEvent(`action.${action.type}`, () => {
        return (draft: Draft<State>) => {
          (StateActions as any)[action.type](draft, action.args);
        };
      }, {
        args: flattenArgs(action.args),
      });
    });
    globals.store.edit(edits);
  }
}

function setExtensionAvailability(available: boolean) {
  globals.dispatch(Actions.setExtensionAvailable({
    available,
  }));
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
      'http://127.0.0.1:9001',  // For trace_processor_shell --httpd.
      'ws://127.0.0.1:9001',    // Ditto, for the websocket RPC.
      'ws://127.0.0.1:8037',    // For the adb websocket server.
      'https://*.google-analytics.com',
      'https://*.googleapis.com',  // For Google Cloud Storage fetches.
      'blob:',
      'data:',
    ],
    'img-src': [
      `'self'`,
      'data:',
      'blob:',
      'https://*.google-analytics.com',
      'https://www.googletagmanager.com',
      'https://*.googleapis.com',
    ],
    'style-src': [
      `'self'`,
      `'unsafe-inline'`,
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
  const favicon = document.head.querySelector('#favicon') as HTMLLinkElement;
  if (favicon) favicon.href = globals.root + 'assets/favicon.png';

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

  // Add Error handlers for JS error and for uncaught exceptions in promises.
  setErrorHandler((err: string) => maybeShowErrorDialog(err));
  window.addEventListener('error', (e) => reportError(e));
  window.addEventListener('unhandledrejection', (e) => reportError(e));

  const extensionLocalChannel = new MessageChannel();

  initWasm(globals.root);
  initController(extensionLocalChannel.port1);

  const dispatch = (action: DeferredAction) => {
    frontendApi.dispatchMultiple([action]);
  };

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
  });
  router.onRouteChanged = routeChange;

  // These need to be set before globals.initialize.
  const route = Router.parseUrl(window.location.href);
  globals.embeddedMode = route.args.mode === 'embedded';
  globals.hideSidebar = route.args.hideSidebar === true;

  const cmdManager = new CommandManager();

  globals.initialize(dispatch, router, createEmptyState(), cmdManager);

  globals.serviceWorkerController.install();

  const frontendApi = new FrontendApi();
  globals.publishRedraw = () => raf.scheduleFullRedraw();

  // We proxy messages between the extension and the controller because the
  // controller's worker can't access chrome.runtime.
  const extensionPort = window.chrome && chrome.runtime ?
      chrome.runtime.connect(EXTENSION_ID) :
      undefined;

  setExtensionAvailability(extensionPort !== undefined);

  if (extensionPort) {
    extensionPort.onDisconnect.addListener((_) => {
      setExtensionAvailability(false);
      void chrome.runtime.lastError;  // Needed to not receive an error log.
    });
    // This forwards the messages from the extension to the controller.
    extensionPort.onMessage.addListener(
        (message: object, _port: chrome.runtime.Port) => {
          if (isGetCategoriesResponse(message)) {
            globals.dispatch(Actions.setChromeCategories(message));
            return;
          }
          extensionLocalChannel.port2.postMessage(message);
        });
  }

  // This forwards the messages from the controller to the extension
  extensionLocalChannel.port2.onmessage = ({data}) => {
    if (extensionPort) extensionPort.postMessage(data);
  };

  // Put debug variables in the global scope for better debugging.
  registerDebugGlobals();

  // Prevent pinch zoom.
  document.body.addEventListener('wheel', (e: MouseEvent) => {
    if (e.ctrlKey) e.preventDefault();
  }, {passive: false});

  cssLoadPromise.then(() => onCssLoaded());

  if (globals.testing) {
    document.body.classList.add('testing');
  }

  // Initialize all plugins:
  const viewer = new ViewerImpl();
  for (const plugin of pluginRegistry.values()) {
    pluginManager.activatePlugin(plugin.pluginId, viewer);
  }

  cmdManager.registerCommandSource(pluginManager);
}

function onCssLoaded() {
  initCssConstants();
  // Clear all the contents of the initial page (e.g. the <pre> error message)
  // And replace it with the root <main> element which will be used by mithril.
  document.body.innerHTML = '';

  raf.domRedraw = () => {
    m.render(document.body, m(App, globals.router.resolve()));
  };

  initLiveReloadIfLocalhost();

  if (!RECORDING_V2_FLAG.get()) {
    updateAvailableAdbDevices();
    try {
      navigator.usb.addEventListener(
          'connect', () => updateAvailableAdbDevices());
      navigator.usb.addEventListener(
          'disconnect', () => updateAvailableAdbDevices());
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
  CheckHttpRpcConnection().then(() => {
    const route = Router.parseUrl(window.location.href);

    globals.dispatch(Actions.maybeSetPendingDeeplink({
      ts: route.args.ts,
      tid: route.args.tid,
      dur: route.args.dur,
      pid: route.args.pid,
      query: route.args.query,
      visStart: route.args.visStart,
      visEnd: route.args.visEnd,
    }));

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
}

main();
