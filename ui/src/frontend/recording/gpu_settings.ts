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

import {Probe, ProbeAttrs} from '../record_widgets';
import {RecordingSectionAttrs} from './recording_sections';

export class GpuSettings implements m.ClassComponent<RecordingSectionAttrs> {
  view({attrs}: m.CVnode<RecordingSectionAttrs>) {
    return m(
        `.record-section${attrs.cssClass}`,
        m(Probe, {
          title: 'GPU frequency',
          img: 'rec_cpu_freq.png',
          descr: 'Records gpu frequency via ftrace',
          setEnabled: (cfg, val) => cfg.gpuFreq = val,
          isEnabled: (cfg) => cfg.gpuFreq,
        } as ProbeAttrs),
        m(Probe, {
          title: 'GPU memory',
          img: 'rec_gpu_mem_total.png',
          descr:
              `Allows to track per process and global total GPU memory usages.
                (Available on recent Android 12+ kernels)`,
          setEnabled: (cfg, val) => cfg.gpuMemTotal = val,
          isEnabled: (cfg) => cfg.gpuMemTotal,
        } as ProbeAttrs));
  }
}
