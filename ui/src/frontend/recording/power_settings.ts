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

import * as m from 'mithril';

import {globals} from '../globals';
import {Probe, ProbeAttrs, Slider, SliderAttrs} from '../record_widgets';
import {POLL_INTERVAL_MS, RecordingSectionAttrs} from './recording_sections';

export class PowerSettings implements m.ClassComponent<RecordingSectionAttrs> {
  view({attrs}: m.CVnode<RecordingSectionAttrs>) {
    const DOC_URL = 'https://perfetto.dev/docs/data-sources/battery-counters';
    const descr =
        [m('div',
           m('span', `Polls charge counters and instantaneous power draw from
                    the battery power management IC and the power rails from
                    the PowerStats HAL (`),
           m('a', {href: DOC_URL, target: '_blank'}, 'see docs for more'),
           m('span', ')'))];
    if (globals.isInternalUser) {
      descr.push(m(
          'div',
          m('span', 'Googlers: See '),
          m('a',
            {href: 'http://go/power-rails-internal-doc', target: '_blank'},
            'this doc'),
          m('span',
            ` for instructions on how to change the refault rail selection
                  on internal devices.`),
          ));
    }
    return m(
        `.record-section${attrs.cssClass}`,
        m(Probe,
          {
            title: 'Battery drain & power rails',
            img: 'rec_battery_counters.png',
            descr,
            setEnabled: (cfg, val) => cfg.batteryDrain = val,
            isEnabled: (cfg) => cfg.batteryDrain,
          } as ProbeAttrs,
          m(Slider, {
            title: 'Poll interval',
            cssClass: '.thin',
            values: POLL_INTERVAL_MS,
            unit: 'ms',
            set: (cfg, val) => cfg.batteryDrainPollMs = val,
            get: (cfg) => cfg.batteryDrainPollMs,
          } as SliderAttrs)),
        m(Probe, {
          title: 'Board voltages & frequencies',
          img: 'rec_board_voltage.png',
          descr: 'Tracks voltage and frequency changes from board sensors',
          setEnabled: (cfg, val) => cfg.boardSensors = val,
          isEnabled: (cfg) => cfg.boardSensors,
        } as ProbeAttrs));
  }
}
