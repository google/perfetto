// Copyright (C) 2024 The Android Open Source Project
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
import {Probe} from '../record_widgets';
import {RecordingSectionAttrs} from './recording_sections';

export class EtwSettings implements m.ClassComponent<RecordingSectionAttrs> {
  view({attrs}: m.CVnode<RecordingSectionAttrs>) {
    const recCfg = attrs.recState.recordConfig;
    return m(
      `.record-section${attrs.cssClass}`,
      m(Probe, {
        title: 'CSwitch',
        img: null,
        descr: `Enables to recording of context switches.`,
        setEnabled: (cfg, val) => (cfg.etwCSwitch = val),
        isEnabled: (cfg) => cfg.etwCSwitch,
        recCfg,
      }),
      m(Probe, {
        title: 'Dispatcher',
        img: null,
        descr: 'Enables to get thread state.',
        setEnabled: (cfg, val) => (cfg.etwThreadState = val),
        isEnabled: (cfg) => cfg.etwThreadState,
        recCfg,
      }),
    );
  }
}
