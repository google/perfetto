// Copyright (C) 2022 The Android Open Source Project
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

import {produce} from 'immer';
import m from 'mithril';

import {Actions} from '../../common/actions';
import {RecordMode} from '../../common/state';
import {globals} from '../globals';
import {Slider, SliderAttrs} from '../record_widgets';

import {RecordingSectionAttrs} from './recording_sections';

export class RecordingSettings implements
    m.ClassComponent<RecordingSectionAttrs> {
  view({attrs}: m.CVnode<RecordingSectionAttrs>) {
    const S = (x: number) => x * 1000;
    const M = (x: number) => x * 1000 * 60;
    const H = (x: number) => x * 1000 * 60 * 60;

    const cfg = globals.state.recordConfig;

    const recButton = (mode: RecordMode, title: string, img: string) => {
      const checkboxArgs = {
        checked: cfg.mode === mode,
        onchange: (e: InputEvent) => {
          const checked = (e.target as HTMLInputElement).checked;
          if (!checked) return;
          const traceCfg = produce(globals.state.recordConfig, (draft) => {
            draft.mode = mode;
          });
          globals.dispatch(Actions.setRecordConfig({config: traceCfg}));
        },
      };
      return m(
          `label${cfg.mode === mode ? '.selected' : ''}`,
          m(`input[type=radio][name=rec_mode]`, checkboxArgs),
          m(`img[src=${globals.root}assets/${img}]`),
          m('span', title));
    };

    return m(
        `.record-section${attrs.cssClass}`,
        m('header', 'Recording mode'),
        m('.record-mode',
          recButton('STOP_WHEN_FULL', 'Stop when full', 'rec_one_shot.png'),
          recButton('RING_BUFFER', 'Ring buffer', 'rec_ring_buf.png'),
          recButton('LONG_TRACE', 'Long trace', 'rec_long_trace.png')),

        m(Slider, {
          title: 'In-memory buffer size',
          icon: '360',
          values: [4, 8, 16, 32, 64, 128, 256, 512],
          unit: 'MB',
          set: (cfg, val) => cfg.bufferSizeMb = val,
          get: (cfg) => cfg.bufferSizeMb,
        } as SliderAttrs),

        m(Slider, {
          title: 'Max duration',
          icon: 'timer',
          values: [S(10), S(15), S(30), S(60), M(5), M(30), H(1), H(6), H(12)],
          isTime: true,
          unit: 'h:m:s',
          set: (cfg, val) => cfg.durationMs = val,
          get: (cfg) => cfg.durationMs,
        } as SliderAttrs),
        m(Slider, {
          title: 'Max file size',
          icon: 'save',
          cssClass: cfg.mode !== 'LONG_TRACE' ? '.hide' : '',
          values: [5, 25, 50, 100, 500, 1000, 1000 * 5, 1000 * 10],
          unit: 'MB',
          set: (cfg, val) => cfg.maxFileSizeMb = val,
          get: (cfg) => cfg.maxFileSizeMb,
        } as SliderAttrs),
        m(Slider, {
          title: 'Flush on disk every',
          cssClass: cfg.mode !== 'LONG_TRACE' ? '.hide' : '',
          icon: 'av_timer',
          values: [100, 250, 500, 1000, 2500, 5000],
          unit: 'ms',
          set: (cfg, val) => cfg.fileWritePeriodMs = val,
          get: (cfg) => cfg.fileWritePeriodMs || 0,
        } as SliderAttrs));
  }
}
