/*
 * Copyright (C) 2018 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

import * as m from 'mithril';
import {frontend} from './frontend';

console.log('Hello from the main thread!');

function createController() {
  const worker = new Worker("worker_bundle.js");
  worker.onerror = e => {
    console.error(e);
  };
}

function createFrontend() {
  const root = document.getElementById('frontend');
  if (!root) {
    console.error('root element not found.');
    return;
  }
  const rect = root.getBoundingClientRect();

  m.render(root, m(frontend, {
    width: rect.width,
    height: rect.height
  }));
}

function main() {
  createController();
  createFrontend();
}

main();
