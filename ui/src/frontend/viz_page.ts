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
import {raf} from '../core/raf_scheduler';
import {Editor} from '../widgets/editor';
import {VegaView} from '../widgets/vega_view';
import {PageWithTraceAttrs} from './pages';
import {assertExists} from '../base/logging';
import {Engine} from '../trace_processor/engine';

let SPEC = '';

export class VizPage implements m.ClassComponent<PageWithTraceAttrs> {
  private engine?: Engine;

  oninit({attrs}: m.CVnode<PageWithTraceAttrs>) {
    this.engine = attrs.trace.engine.getProxy('VizPage');
  }

  view() {
    const engine = assertExists(this.engine);
    return m(
      '.viz-page',
      m(VegaView, {
        spec: SPEC,
        engine: engine,
        data: {},
      }),
      m(Editor, {
        onUpdate: (text: string) => {
          SPEC = text;
          raf.scheduleFullRedraw();
        },
      }),
    );
  }
}
