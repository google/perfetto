// Copyright (C) 2023 The Android Open Source Project
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

import {EngineProxy} from '../common/engine';
import {raf} from '../core/raf_scheduler';
import {Editor} from '../widgets/editor';

import {globals} from './globals';
import {createPage} from './pages';
import {VegaView} from './widgets/vega_view';

function getEngine(): EngineProxy|undefined {
  const engineId = globals.getCurrentEngine()?.id;
  if (engineId === undefined) {
    return undefined;
  }
  const engine = globals.engines.get(engineId)?.getProxy('VizPage');
  return engine;
}


let SPEC = '';
let ENGINE: EngineProxy|undefined = undefined;

export const VizPage = createPage({
  oncreate() {
    ENGINE = getEngine();
  },

  view() {
    return m(
        '.viz-page',
        m(VegaView, {
          spec: SPEC,
          engine: ENGINE,
          data: {},
        }),
        m(Editor, {
          onUpdate: (text: string) => {
            SPEC = text;
            raf.scheduleFullRedraw();
          },
        }),
    );
  },
});
