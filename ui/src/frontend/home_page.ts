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

import {WasmEngineProxy} from '../controller/wasm_engine_proxy';

import {gEngines, globals} from './globals';
import {quietDispatch} from './mithril_helpers';
import {createPage} from './pages';

function extractBlob(e: Event): Blob|null {
  if (!(e.target instanceof HTMLInputElement)) {
    throw new Error('Not input element');
  }
  if (!e.target.files) return null;
  return e.target.files.item(0);
}

async function loadExampleTrace() {
  const url = 'https://storage.googleapis.com/perfetto-misc/example_trace';
  const repsonse = await fetch(url);
  const blob = await repsonse.blob();
  gEngines.set('0', WasmEngineProxy.create(blob));
  m.route.set('/query/0');
}

export const HomePage = createPage({
  view() {
    const count = globals.state.i;
    return m(
        '.home-page',
        m('.home-page-title', 'Perfetto'),
        m('.home-page-controls',
          m('label.file-input',
            m('input[type=file]', {
              onchange: (e: Event) => {
                const blob = extractBlob(e);
                if (!blob) return;
                gEngines.set('0', WasmEngineProxy.create(blob));
                m.route.set('/query/0');
              },
            }),
            'Load trace'),
          ' or ',
          m('button', {onclick: loadExampleTrace}, 'Open demo trace'),
          m('button', {onclick: quietDispatch({})}, `Increment ${count}`)));
  }
});
