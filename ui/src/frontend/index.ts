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

import {applyPatches, Patch} from 'immer';
import * as m from 'mithril';

import {forwardRemoteCalls} from '../base/remote';
import {Actions} from '../common/actions';

import {globals, QuantizedLoad, SliceDetails, ThreadDesc} from './globals';
import {HomePage} from './home_page';
import {openBufferWithLegacyTraceViewer} from './legacy_trace_viewer';
import {RecordPage} from './record_page';
import {Router} from './router';
import {ViewerPage} from './viewer_page';

/**
 * The API the main thread exposes to the controller.
 */
class FrontendApi {
  constructor(private router: Router) {}

  patchState(patches: Patch[]) {
    globals.state = applyPatches(globals.state, patches);
    // If the visible time in the global state has been updated more recently
    // than the visible time handled by the frontend @ 60fps, update it. This
    // typically happens when restoring the state from a permalink.
    globals.frontendLocalState.mergeState(globals.state.frontendLocalState);
    this.redraw();
  }

  // TODO: we can't have a publish method for each batch of data that we don't
  // want to keep in the global state. Figure out a more generic and type-safe
  // mechanism to achieve this.

  publishOverviewData(data: {[key: string]: QuantizedLoad | QuantizedLoad[]}) {
    for (const [key, value] of Object.entries(data)) {
      if (!globals.overviewStore.has(key)) {
        globals.overviewStore.set(key, []);
      }
      if (value instanceof Array) {
        globals.overviewStore.get(key)!.push(...value);
      } else {
        globals.overviewStore.get(key)!.push(value);
      }
    }
    globals.rafScheduler.scheduleRedraw();
  }

  publishTrackData(args: {id: string, data: {}}) {
    globals.setTrackData(args.id, args.data);
    globals.rafScheduler.scheduleRedraw();
  }

  publishQueryResult(args: {id: string, data: {}}) {
    globals.queryResults.set(args.id, args.data);
    this.redraw();
  }

  publishThreads(data: ThreadDesc[]) {
    globals.threads.clear();
    data.forEach(thread => {
      globals.threads.set(thread.utid, thread);
    });
    this.redraw();
  }

  publishSliceDetails(click: SliceDetails) {
    globals.sliceDetails = click;
    this.redraw();
  }

  // For opening JSON/HTML traces with the legacy catapult viewer.
  publishLegacyTrace(args: {data: ArrayBuffer, size: number}) {
    const arr = new Uint8Array(args.data, 0, args.size);
    const str = (new TextDecoder('utf-8')).decode(arr);
    openBufferWithLegacyTraceViewer('trace.json', str, 0);
  }

  private redraw(): void {
    if (globals.state.route &&
        globals.state.route !== this.router.getRouteFromHash()) {
      this.router.setRouteOnHash(globals.state.route);
    }

    globals.rafScheduler.scheduleFullRedraw();
  }
}

function main() {
  const controller = new Worker('controller_bundle.js');
  controller.onerror = e => {
    console.error(e);
  };
  const channel = new MessageChannel();
  controller.postMessage(channel.port1, [channel.port1]);
  const dispatch = controller.postMessage.bind(controller);
  const router = new Router(
      '/',
      {
        '/': HomePage,
        '/viewer': ViewerPage,
        '/record': RecordPage,
      },
      dispatch);
  forwardRemoteCalls(channel.port2, new FrontendApi(router));
  globals.initialize(dispatch, controller);

  globals.rafScheduler.domRedraw = () =>
      m.render(document.body, m(router.resolve(globals.state.route)));


  // Put these variables in the global scope for better debugging.
  (window as {} as {m: {}}).m = m;
  (window as {} as {globals: {}}).globals = globals;

  // /?s=xxxx for permalinks.
  const stateHash = Router.param('s');
  if (stateHash) {
    globals.dispatch(Actions.loadPermalink({
      hash: stateHash,
    }));
  }

  // Prevent pinch zoom.
  document.body.addEventListener('wheel', (e: MouseEvent) => {
    if (e.ctrlKey) e.preventDefault();
  });

  router.navigateToCurrentHash();
}

main();
