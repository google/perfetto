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

import '../tracks/all_frontend';

import {applyPatches, Patch, produce} from 'immer';
import * as m from 'mithril';

import {defer} from '../base/deferred';
import {assertExists, reportError, setErrorHandler} from '../base/logging';
import {forwardRemoteCalls} from '../base/remote';
import {Actions, DeferredAction, StateActions} from '../common/actions';
import {tryGetTrace} from '../common/cache_manager';
import {initializeImmerJs} from '../common/immer_init';
import {createEmptyState, State} from '../common/state';
import {initWasm} from '../common/wasm_engine_proxy';
import {ControllerWorkerInitMessage} from '../common/worker_messages';
import {initController} from '../controller/index';

import {AnalyzePage} from './analyze_page';
import {loadAndroidBugToolInfo} from './android_bug_tool';
import {initCssConstants} from './css_constants';
import {maybeShowErrorDialog} from './error_dialog';
import {installFileDropHandler} from './file_drop_handler';
import {FlagsPage} from './flags_page';
import {globals} from './globals';
import {HomePage} from './home_page';
import {initLiveReloadIfLocalhost} from './live_reload';
import {MetricsPage} from './metrics_page';
import {PageAttrs} from './pages';
import {postMessageHandler} from './post_message_handler';
import {RecordPage, updateAvailableAdbDevices} from './record_page';
import {Router} from './router';
import {CheckHttpRpcConnection} from './rpc_http_dialog';
import {taskTracker} from './task_tracker';
import {TraceInfoPage} from './trace_info_page';
import {ViewerPage} from './viewer_page';

const EXTENSION_ID = 'lfmkphfpdbjijhpomgecfikhfohaoine';

function isLocalhostTraceUrl(url: string): boolean {
  return ['127.0.0.1', 'localhost'].includes((new URL(url)).hostname);
}

/**
 * The API the main thread exposes to the controller.
 */
class FrontendApi {
  private router: Router;
  private port: MessagePort;
  private state: State;

  constructor(router: Router, port: MessagePort) {
    this.router = router;
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
    for (const key in this.state) {
      if (key !== 'frontendLocalState' && key !== 'visibleTracks' &&
          oldState[key] !== this.state[key]) {
        this.redraw();
        break;
      }
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
        draft => {
          // tslint:disable-next-line no-any
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

  patchState(patches: Patch[]) {
    const oldState = globals.state;
    globals.state = applyPatches(globals.state, patches);

    // If the visible time in the global state has been updated more recently
    // than the visible time handled by the frontend @ 60fps, update it. This
    // typically happens when restoring the state from a permalink.
    globals.frontendLocalState.mergeState(globals.state.frontendLocalState);

    // Only redraw if something other than the frontendLocalState changed.
    for (const key in globals.state) {
      if (key !== 'frontendLocalState' && key !== 'visibleTracks' &&
          oldState[key] !== globals.state[key]) {
        this.redraw();
        return;
      }
    }
  }

  redraw(): void {
    const traceIdString =
        globals.state.traceUuid ? `?trace_id=${globals.state.traceUuid}` : '';
    if (globals.state.route &&
        globals.state.route + traceIdString !==
            this.router.getFullRouteFromHash()) {
      this.router.setRouteOnHash(globals.state.route, traceIdString);
    }
    globals.rafScheduler.scheduleFullRedraw();
  }
}

function setExtensionAvailability(available: boolean) {
  globals.dispatch(Actions.setExtensionAvailable({
    available,
  }));
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
      'https://*.google.com',
      'https://*.googleusercontent.com',
      'https://www.googletagmanager.com',
      'https://www.google-analytics.com',
    ],
    'object-src': ['none'],
    'connect-src': [
      `'self'`,
      'http://127.0.0.1:9001',  // For trace_processor_shell --httpd.
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
  window.addEventListener('error', e => reportError(e));
  window.addEventListener('unhandledrejection', e => reportError(e));

  const frontendChannel = new MessageChannel();
  const controllerChannel = new MessageChannel();
  const extensionLocalChannel = new MessageChannel();
  const errorReportingChannel = new MessageChannel();

  errorReportingChannel.port2.onmessage = (e) =>
      maybeShowErrorDialog(`${e.data}`);

  const msg: ControllerWorkerInitMessage = {
    frontendPort: frontendChannel.port1,
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

  globals.initialize(dispatch);
  globals.serviceWorkerController.install();

  const routes = new Map<string, m.Component<PageAttrs>>();
  routes.set('/', HomePage);
  routes.set('/viewer', ViewerPage);
  routes.set('/record', RecordPage);
  routes.set('/query', AnalyzePage);
  routes.set('/flags', FlagsPage);
  routes.set('/metrics', MetricsPage);
  routes.set('/info', TraceInfoPage);
  const router = new Router('/', routes, dispatch, globals.logging);
  const frontendApi = new FrontendApi(router, controllerChannel.port2);
  globals.publishRedraw = () => frontendApi.redraw();
  forwardRemoteCalls(frontendChannel.port2, frontendApi);

  // We proxy messages between the extension and the controller because the
  // controller's worker can't access chrome.runtime.
  const extensionPort = window.chrome && chrome.runtime ?
      chrome.runtime.connect(EXTENSION_ID) :
      undefined;

  setExtensionAvailability(extensionPort !== undefined);

  if (extensionPort) {
    extensionPort.onDisconnect.addListener(_ => {
      setExtensionAvailability(false);
      // tslint:disable-next-line: no-unused-expression
      void chrome.runtime.lastError;  // Needed to not receive an error log.
    });
    // This forwards the messages from the extension to the controller.
    extensionPort.onMessage.addListener(
        (message: object, _port: chrome.runtime.Port) => {
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

  cssLoadPromise.then(() => onCssLoaded(router));

  if (globals.testing) {
    document.body.classList.add('testing');
  }
}

function onCssLoaded(router: Router) {
  initCssConstants();
  // Clear all the contents of the initial page (e.g. the <pre> error message)
  // And replace it with the root <main> element which will be used by mithril.
  document.body.innerHTML = '<main></main>';
  const main = assertExists(document.body.querySelector('main'));
  globals.rafScheduler.domRedraw = () => {
    m.render(main, router.resolve(globals.state.route));
  };

  /**
   * Start of hack for backwards compatibility:
   * There are some old URLs in the form of 'record?p=power'. We want these
   * to keep opening the desired page(see b/191255021#comment2).
   */
  if (window.location.hash.startsWith('#!/record?p=')) {
    window.location.hash = window.location.hash.replace('?p=', '/');
  }
  // end of hack for backwards compatibility

  router.navigateToCurrentHash();

  // /?s=xxxx for permalinks.
  const stateHash = Router.param('s');
  const traceUuid = Router.param('trace_id');
  const urlHash = Router.param('url');
  const androidBugTool = Router.param('openFromAndroidBugTool');
  if (typeof stateHash === 'string' && stateHash) {
    globals.dispatch(Actions.loadPermalink({
      hash: stateHash,
    }));
  } else if (typeof urlHash === 'string' && urlHash) {
    if (isLocalhostTraceUrl(urlHash)) {
      const fileName = urlHash.split('/').pop() || 'local_trace.pftrace';
      const request = fetch(urlHash)
                          .then(response => response.blob())
                          .then(blob => {
                            globals.dispatch(Actions.openTraceFromFile({
                              file: new File([blob], fileName),
                            }));
                          })
                          .catch(e => alert(`Could not load local trace ${e}`));
      taskTracker.trackPromise(request, 'Downloading local trace');
    } else {
      globals.dispatch(Actions.openTraceFromUrl({
        url: urlHash,
      }));
    }
  } else if (androidBugTool) {
    // TODO(hjd): Unify updateStatus and TaskTracker
    globals.dispatch(Actions.updateStatus({
      msg: 'Loading trace from ABT extension',
      timestamp: Date.now() / 1000
    }));
    const loadInfo = loadAndroidBugToolInfo();
    taskTracker.trackPromise(loadInfo, 'Loading trace from ABT extension');
    loadInfo
        .then(info => {
          globals.dispatch(Actions.openTraceFromFile({
            file: info.file,
          }));
        })
        .catch(e => {
          console.error(e);
        });
  } else if (traceUuid) {
    maybeLoadCachedTrace(traceUuid);
  }

  // Add support for opening traces from postMessage().
  window.addEventListener('message', postMessageHandler, {passive: true});

  // Will update the chip on the sidebar footer that notifies that the RPC is
  // connected. Has no effect on the controller (which will repeat this check
  // before creating a new engine).
  CheckHttpRpcConnection();
  initLiveReloadIfLocalhost();

  updateAvailableAdbDevices();
  try {
    navigator.usb.addEventListener(
        'connect', () => updateAvailableAdbDevices());
    navigator.usb.addEventListener(
        'disconnect', () => updateAvailableAdbDevices());
  } catch (e) {
    console.error('WebUSB API not supported');
  }
  installFileDropHandler();
}

async function maybeLoadCachedTrace(traceUuid: string) {
  const trace = await tryGetTrace(traceUuid);
  if (trace === undefined) return;
  globals.dispatch(Actions.openTraceFromBuffer(trace));
}

main();
