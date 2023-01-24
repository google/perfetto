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

import {Patch, produce} from 'immer';
import * as m from 'mithril';

import {defer} from '../base/deferred';
import {assertExists, reportError, setErrorHandler} from '../base/logging';
import {Actions, DeferredAction, StateActions} from '../common/actions';
import {createEmptyState} from '../common/empty_state';
import {RECORDING_V2_FLAG} from '../common/feature_flags';
import {initializeImmerJs} from '../common/immer_init';
import {pluginManager, pluginRegistry} from '../common/plugins';
import {onSelectionChanged} from '../common/selection_observer';
import {State} from '../common/state';
import {initWasm} from '../common/wasm_engine_proxy';
import {ControllerWorkerInitMessage} from '../common/worker_messages';
import {
  isGetCategoriesResponse,
} from '../controller/chrome_proxy_record_controller';
import {initController} from '../controller/index';

import {AnalyzePage} from './analyze_page';
import {initCssConstants} from './css_constants';
import {maybeShowErrorDialog} from './error_dialog';
import {installFileDropHandler} from './file_drop_handler';
import {FlagsPage} from './flags_page';
import {globals} from './globals';
import {HomePage} from './home_page';
import {initLiveReloadIfLocalhost} from './live_reload';
import {MetricsPage} from './metrics_page';
import {postMessageHandler} from './post_message_handler';
import {RecordPage, updateAvailableAdbDevices} from './record_page';
import {RecordPageV2} from './record_page_v2';
import {Router} from './router';
import {CheckHttpRpcConnection} from './rpc_http_dialog';
import {TraceInfoPage} from './trace_info_page';
import {maybeOpenTraceFromRoute} from './trace_url_handler';
import {ViewerPage} from './viewer_page';

const EXTENSION_ID = 'lfmkphfpdbjijhpomgecfikhfohaoine';

class FrontendApi {
  private port: MessagePort;
  private state: State;

  constructor(port: MessagePort) {
    this.state = createEmptyState();
    this.port = port;
  }

  dispatchMultiple(actions: DeferredAction[]) {
    const oldState = this.state;
    const patches: Patch[] = [];
    for (const action of actions) {
      const originalLength = patches.length;
      const morePatches = this.applyAction(action);
      patches.length += morePatches.length;
      for (let i = 0; i < morePatches.length; ++i) {
        patches[i + originalLength] = morePatches[i];
      }
    }

    if (this.state === oldState) {
      return;
    }

    // Update overall state.
    globals.state = this.state;

    // If the visible time in the global state has been updated more recently
    // than the visible time handled by the frontend @ 60fps, update it. This
    // typically happens when restoring the state from a permalink.
    globals.frontendLocalState.mergeState(this.state.frontendLocalState);

    // Only redraw if something other than the frontendLocalState changed.
    let key: keyof State;
    for (key in this.state) {
      if (key !== 'frontendLocalState' && key !== 'visibleTracks' &&
          oldState[key] !== this.state[key]) {
        globals.rafScheduler.scheduleFullRedraw();
        break;
      }
    }

    if (this.state.currentSelection !== oldState.currentSelection) {
      // TODO(altimin): Currently we are not triggering this when changing
      // the set of selected tracks via toggling per-track checkboxes.
      // Fix that.
      onSelectionChanged(
          this.state.currentSelection || undefined,
          oldState.currentSelection || undefined);
    }

    if (patches.length > 0) {
      this.port.postMessage(patches);
    }
  }

  private applyAction(action: DeferredAction): Patch[] {
    const patches: Patch[] = [];

    // 'produce' creates a immer proxy which wraps the current state turning
    // all imperative mutations of the state done in the callback into
    // immutable changes to the returned state.
    this.state = produce(
        this.state,
        (draft) => {
          (StateActions as any)[action.type](draft, action.args);
        },
        (morePatches, _) => {
          const originalLength = patches.length;
          patches.length += morePatches.length;
          for (let i = 0; i < morePatches.length; ++i) {
            patches[i + originalLength] = morePatches[i];
          }
        });
    return patches;
  }
}

function setExtensionAvailability(available: boolean) {
  globals.dispatch(Actions.setExtensionAvailable({
    available,
  }));
}

function initGlobalsFromQueryString() {
  const queryString = window.location.search;
  globals.embeddedMode = queryString.includes('mode=embedded');
  globals.hideSidebar = queryString.includes('hideSidebar=true');
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
      'https://www.google-analytics.com',
    ],
    'object-src': ['none'],
    'connect-src': [
      `'self'`,
      'http://127.0.0.1:9001',  // For trace_processor_shell --httpd.
      'ws://127.0.0.1:9001',    // Ditto, for the websocket RPC.
      'ws://127.0.0.1:8037',    // For the adb websocket server.
      'https://www.google-analytics.com',
      'https://*.googleapis.com',  // For Google Cloud Storage fetches.
      'blob:',
      'data:',
    ],
    'img-src': [
      `'self'`,
      'data:',
      'blob:',
      'https://www.google-analytics.com',
      'https://www.googletagmanager.com',
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
  // appenChild returns.
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

  const controllerChannel = new MessageChannel();
  const extensionLocalChannel = new MessageChannel();
  const errorReportingChannel = new MessageChannel();

  errorReportingChannel.port2.onmessage = (e) =>
      maybeShowErrorDialog(`${e.data}`);

  const msg: ControllerWorkerInitMessage = {
    controllerPort: controllerChannel.port1,
    extensionPort: extensionLocalChannel.port1,
    errorReportingPort: errorReportingChannel.port1,
  };

  initWasm(globals.root);
  initializeImmerJs();

  initController(msg);

  const dispatch = (action: DeferredAction) => {
    frontendApi.dispatchMultiple([action]);
  };

  const router = new Router({
    '/': HomePage,
    '/viewer': ViewerPage,
    '/record': RECORDING_V2_FLAG.get() ? RecordPageV2 : RecordPage,
    '/query': AnalyzePage,
    '/flags': FlagsPage,
    '/metrics': MetricsPage,
    '/info': TraceInfoPage,
  });
  router.onRouteChanged = (route) => {
    globals.rafScheduler.scheduleFullRedraw();
    maybeOpenTraceFromRoute(route);
  };

  // This must be called before calling `globals.initialize` so that the
  // `embeddedMode` global is set.
  initGlobalsFromQueryString();

  globals.initialize(dispatch, router);
  globals.serviceWorkerController.install();

  const frontendApi = new FrontendApi(controllerChannel.port2);
  globals.publishRedraw = () => globals.rafScheduler.scheduleFullRedraw();

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

  // Put these variables in the global scope for better debugging.
  (window as {} as {m: {}}).m = m;
  (window as {} as {globals: {}}).globals = globals;
  (window as {} as {Actions: {}}).Actions = Actions;

  // Prevent pinch zoom.
  document.body.addEventListener('wheel', (e: MouseEvent) => {
    if (e.ctrlKey) e.preventDefault();
  }, {passive: false});

  cssLoadPromise.then(() => onCssLoaded());

  if (globals.testing) {
    document.body.classList.add('testing');
  }

  // Initialize all plugins:
  for (const plugin of pluginRegistry.values()) {
    pluginManager.activatePlugin(plugin.pluginId);
  }
}


function onCssLoaded() {
  initCssConstants();
  // Clear all the contents of the initial page (e.g. the <pre> error message)
  // And replace it with the root <main> element which will be used by mithril.
  document.body.innerHTML = '<main></main>';
  const main = assertExists(document.body.querySelector('main'));
  globals.rafScheduler.domRedraw = () => {
    m.render(main, globals.router.resolve());
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
    maybeOpenTraceFromRoute(Router.parseUrl(window.location.href));
  });
}

main();
