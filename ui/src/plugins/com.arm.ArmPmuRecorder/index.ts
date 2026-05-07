// Copyright (C) 2026 The Android Open Source Project
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

import {App} from '../../public/app';
import {PerfettoPlugin} from '../../public/plugin';
import RecordTraceV2Plugin, {
  registerRecordSubpageProvider,
} from '../dev.perfetto.RecordTraceV2';
import ArmTelemetrySpecPlugin from '../com.arm.ArmTelemetrySpec';
import {pmuRecordSection} from './pmu';

export default class implements PerfettoPlugin {
  static readonly id = 'com.arm.ArmPmuRecorder';
  static readonly description = 'Arm PMU recording page';
  static readonly dependencies = [RecordTraceV2Plugin, ArmTelemetrySpecPlugin];

  static onActivate(app: App): void {
    registerRecordSubpageProvider((recMgr) => pmuRecordSection(recMgr, app));
  }
}
