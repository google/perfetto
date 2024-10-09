// Copyright (C) 2021 The Android Open Source Project
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
import {tryGetTrace} from '../core/cache_manager';
import {showModal} from '../widgets/modal';
import {loadPermalink} from './permalink';
import {loadAndroidBugToolInfo} from './android_bug_tool';
import {Route} from '../core/router';
import {taskTracker} from './task_tracker';
import {AppImpl} from '../core/app_impl';
import {Actions} from '../common/actions';
import {globals} from './globals';
import {Router} from '../core/router';

function getCurrentTraceUrl(): undefined | string {
  const source = AppImpl.instance.trace?.traceInfo.source;
  if (source && source.type === 'URL') {
    return source.url;
  }
  return undefined;
}

export function maybeOpenTraceFromRoute(route: Route) {
  if (route.args.s) {
    // /?s=xxxx for permalinks.
    loadPermalink(route.args.s);
    return;
  }

  const url = route.args.url;
  if (url && url !== getCurrentTraceUrl()) {
    // /?url=https://commondatastorage.googleapis.com/bucket/trace
    // This really works only for GCS because the Content Security Policy
    // forbids any other url.
    loadTraceFromUrl(url);
    return;
  }

  if (route.args.openFromAndroidBugTool) {
    // Handles interaction with the Android Bug Tool extension. See b/163421158.
    openTraceFromAndroidBugTool();
    return;
  }

  if (route.args.p && route.page === '/record') {
    // Handles backwards compatibility for old URLs (linked from various docs),
    // generated before we switched URL scheme. e.g., 'record?p=power' vs
    // 'record/power'. See b/191255021#comment2.
    Router.navigate(`#!/record/${route.args.p}`);
    return;
  }

  if (route.args.local_cache_key) {
    // Handles the case of loading traces from the cache storage.
    maybeOpenCachedTrace(route.args.local_cache_key);
    return;
  }
}

/*
 * openCachedTrace(uuid) is called: (1) on startup, from frontend/index.ts; (2)
 * every time the fragment changes (from Router.onRouteChange).
 * This function must be idempotent (imagine this is called on every frame).
 * It must take decision based on the app state, not on URL change events.
 * Fragment changes are handled by the union of Router.onHashChange() and this
 * function, as follows:
 * 1. '' -> URL without a ?local_cache_key=xxx arg:
 *  - no effect (except redrawing)
 * 2. URL without local_cache_key -> URL with local_cache_key:
 *  - Load cached trace (without prompting any dialog).
 *  - Show a (graceful) error dialog in the case of cache misses.
 * 3. '' -> URL with a ?local_cache_key=xxx arg:
 *  - Same as case 2.
 * 4. URL with local_cache_key=1 -> URL with local_cache_key=2:
 *  a) If 2 != uuid of the trace currently loaded (TraceImpl.traceInfo.uuid):
 *  - Ask the user if they intend to switch trace and load 2.
 *  b) If 2 == uuid of current trace (e.g., after a new trace has loaded):
 *  - no effect (except redrawing).
 * 5. URL with local_cache_key -> URL without local_cache_key:
 *  - Redirect to ?local_cache_key=1234 where 1234 is the UUID of the previous
 *    URL (this might or might not match traceInfo.uuid).
 *
 * Backward navigation cases:
 * 6. URL without local_cache_key <- URL with local_cache_key:
 *  - Same as case 5.
 * 7. URL with local_cache_key=1 <- URL with local_cache_key=2:
 *  - Same as case 4a: go back to local_cache_key=1 but ask the user to confirm.
 * 8. landing page <- URL with local_cache_key:
 *  - Same as case 5: re-append the local_cache_key.
 */
async function maybeOpenCachedTrace(traceUuid: string) {
  const curTrace = AppImpl.instance.trace?.traceInfo;
  const curCacheUuid = curTrace?.cached ? curTrace.uuid : '';

  if (traceUuid === curCacheUuid) {
    // Do nothing, matches the currently loaded trace.
    return;
  }

  if (traceUuid === '') {
    // This can happen if we switch from an empty UI state to an invalid UUID
    // (e.g. due to a cache miss, below). This can also happen if the user just
    // types /#!/viewer?local_cache_key=.
    return;
  }

  // This handles the case when a trace T1 is loaded and then the url is set to
  // ?local_cache_key=T2. In that case globals.state.traceUuid remains set to T1
  // until T2 has been loaded by the trace processor (can take several seconds).
  // This early out prevents to re-trigger the openTraceFromXXX() action if the
  // URL changes (e.g. if the user navigates back/fwd) while the new trace is
  // being loaded.
  if (
    curTrace !== undefined &&
    curTrace.source.type === 'ARRAY_BUFFER' &&
    curTrace.source.uuid === traceUuid
  ) {
    return;
  }

  // Fetch the trace from the cache storage. If available load it. If not, show
  // a dialog informing the user about the cache miss.
  const maybeTrace = await tryGetTrace(traceUuid);

  const navigateToOldTraceUuid = () =>
    Router.navigate(`#!/viewer?local_cache_key=${curCacheUuid}`);

  if (!maybeTrace) {
    showModal({
      title: 'Could not find the trace in the cache storage',
      content: m(
        'div',
        m(
          'p',
          'You are trying to load a cached trace by setting the ' +
            '?local_cache_key argument in the URL.',
        ),
        m('p', "Unfortunately the trace wasn't in the cache storage."),
        m(
          'p',
          "This can happen if a tab was discarded and wasn't opened " +
            'for too long, or if you just mis-pasted the URL.',
        ),
        m('pre', `Trace UUID: ${traceUuid}`),
      ),
    });
    navigateToOldTraceUuid();
    return;
  }

  // If the UI is in a blank state (no trace has been ever opened), just load
  // the trace without showing any further dialog. This is the case of tab
  // discarding, reloading or pasting a url with a local_cache_key in an empty
  // instance.
  if (curTrace === undefined) {
    globals.dispatch(Actions.openTraceFromBuffer(maybeTrace));
    return;
  }

  // If, instead, another trace is loaded, ask confirmation to the user.
  // Switching to another trace clears the UI state. It can be quite annoying to
  // lose the UI state by accidentally navigating back too much.
  let hasOpenedNewTrace = false;

  await showModal({
    title: 'You are about to load a different trace and reset the UI state',
    content: m(
      'div',
      m(
        'p',
        'You are seeing this because you either pasted a URL with ' +
          'a different ?local_cache_key=xxx argument or because you hit ' +
          'the history back/fwd button and reached a different trace.',
      ),
      m(
        'p',
        'If you continue another trace will be loaded and the UI ' +
          'state will be cleared.',
      ),
      m(
        'pre',
        `Old trace: ${curTrace !== undefined ? curCacheUuid : '<no trace>'}\n` +
          `New trace: ${traceUuid}`,
      ),
    ),
    buttons: [
      {
        text: 'Continue',
        id: 'trace_id_open', // Used by tests.
        primary: true,
        action: () => {
          hasOpenedNewTrace = true;
          globals.dispatch(Actions.openTraceFromBuffer(maybeTrace));
        },
      },
      {text: 'Cancel'},
    ],
  });

  if (!hasOpenedNewTrace) {
    // We handle this after the modal await rather than in the cancel button
    // action so this has effect even if the user clicks Esc or clicks outside
    // of the modal dialog and dismisses it.
    navigateToOldTraceUuid();
  }
}

function loadTraceFromUrl(url: string) {
  const isLocalhostTraceUrl = ['127.0.0.1', 'localhost'].includes(
    new URL(url).hostname,
  );

  if (isLocalhostTraceUrl) {
    // This handles the special case of tools/record_android_trace serving the
    // traces from a local webserver and killing it immediately after having
    // seen the HTTP GET request. In those cases store the trace as a file, so
    // when users click on share we don't fail the re-fetch().
    const fileName = url.split('/').pop() ?? 'local_trace.pftrace';
    const request = fetch(url)
      .then((response) => response.blob())
      .then((blob) => {
        globals.dispatch(
          Actions.openTraceFromFile({
            file: new File([blob], fileName),
          }),
        );
      })
      .catch((e) => alert(`Could not load local trace ${e}`));
    taskTracker.trackPromise(request, 'Downloading local trace');
  } else {
    globals.dispatch(Actions.openTraceFromUrl({url}));
  }
}

function openTraceFromAndroidBugTool() {
  const msg = 'Loading trace from ABT extension';
  AppImpl.instance.omnibox.showStatusMessage(msg);
  const loadInfo = loadAndroidBugToolInfo();
  taskTracker.trackPromise(loadInfo, msg);
  loadInfo
    .then((info) => {
      globals.dispatch(
        Actions.openTraceFromFile({
          file: info.file,
        }),
      );
    })
    .catch((e) => {
      console.error(e);
    });
}
