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
import {Editor} from '../../widgets/editor';
import {VegaView} from '../../components/widgets/vega_view';
import {Engine} from '../../trace_processor/engine';
import {Trace} from '../../public/trace';

export interface VizPageAttrs {
  readonly trace: Trace;
  readonly spec: string;
  readonly setSpec: (spec: string) => void;
}

export class VizPage implements m.ClassComponent<VizPageAttrs> {
  private engine: Engine;

  constructor({attrs}: m.CVnode<VizPageAttrs>) {
    this.engine = attrs.trace.engine.getProxy('VizPage');
  }
  view({attrs}: m.CVnode<VizPageAttrs>) {
    return m(
      '.page.viz-page',
      m(VegaView, {
        spec: attrs.spec,
        engine: this.engine,
        data: {},
      }),
      m(Editor, {
        initialText: attrs.spec,
        onUpdate: (text: string) => {
          attrs.setSpec(text);
        },
      }),
    );
  }
}
