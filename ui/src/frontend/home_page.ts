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

import {Engine} from '../controller/engine';
import {WasmEngineProxy} from '../controller/wasm_engine_proxy';

import {createPage} from './pages';

function extractBlob(e: Event): Blob|null {
  if (!(e.target instanceof HTMLInputElement)) {
    throw new Error('Not input element');
  }
  if (!e.target.files) return null;
  return e.target.files.item(0);
}

// TODO(hjd): Temporary while bringing up controller worker.
let engine: Engine|null = null;

export const HomePage = createPage({
  view() {
    return m(
        'div',
        m('input[type=file]', {
          onchange: (e: Event) => {
            const blob = extractBlob(e);
            if (!blob) return;
            engine = WasmEngineProxy.create(blob);
          },
        }),
        m('button',
          {
            disabled: engine === null,
            onclick: () => {
              if (!engine) return;
              engine
                  .rawQuery({
                    sqlQuery: 'select * from sched;',
                  })
                  .then(console.log);
            },
          },
          'Query'));
  }
});
