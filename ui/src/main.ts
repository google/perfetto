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

import * as m from 'mithril';

import {createEmptyState} from './common/state';
import {warmupWasmEngineWorker} from './engine/wasm_engine_proxy';
import {FrontendPage} from './frontend';
import {gState} from './frontend/globals';
import {HomePage} from './frontend/home_page';

console.log('Hello from the main thread!');

function createController() {
  const worker = new Worker('worker_bundle.js');
  worker.onerror = e => {
    console.error(e);
  };
}

function main() {
  gState.set(createEmptyState());
  createController();
  warmupWasmEngineWorker();

  const root = document.getElementById('frontend');
  if (!root) {
    console.error('root element not found.');
    return;
  }

  m.route(root, '/', {
    '/': HomePage,
    '/viewer': FrontendPage,
  });
}

main();
