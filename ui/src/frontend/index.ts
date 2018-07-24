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

import '../tracks/all_tracks';

import * as m from 'mithril';

import {ObjectById, TrackState} from '../common/state';
import {warmupWasmEngineWorker} from '../controller/wasm_engine_proxy';

import {globals} from './globals';
import {HomePage} from './home_page';
import {QueryPage} from './query_page';
import {ViewerPage} from './viewer_page';

function createController(): Worker {
  const worker = new Worker('controller_bundle.js');
  worker.onerror = e => {
    console.error(e);
  };
  worker.onmessage = msg => {
    globals.state = msg.data;
    m.redraw();
  };
  return worker;
}

function getDemoTracks(): ObjectById<TrackState> {
  const tracks: {[key: string]: TrackState;} = {};
  for (let i = 0; i < 10; i++) {
    let trackType;
    // The track type strings here are temporary. They will be supplied by the
    // controller side track implementation.
    if (i % 2 === 0) {
      trackType = 'CpuSliceTrack';
    } else {
      trackType = 'CpuCounterTrack';
    }
    tracks[i] = {
      id: i.toString(),
      type: trackType,
      height: 100,
      name: `Track ${i}`,
    };
  }
  return tracks;
}

function main() {
  globals.state = {i: 0, tracks: getDemoTracks()};
  const worker = createController();
  // tslint:disable-next-line deprecation
  globals.dispatch = action => worker.postMessage(action);
  warmupWasmEngineWorker();

  const root = document.getElementById('frontend');
  if (!root) {
    console.error('root element not found.');
    return;
  }

  m.route(root, '/', {
    '/': HomePage,
    '/viewer': ViewerPage,
    '/query/:trace': QueryPage,
  });
}

main();
