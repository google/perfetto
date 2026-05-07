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
import {Trace} from '../../public/trace';
import {ArmTelemetryManager} from './arm_telemetry_spec_manager';
import {ArmTelemetryManagerImpl} from './arm_telemetry_spec_manager_impl';

export default class ArmTelemetrySpecPlugin implements PerfettoPlugin {
  static readonly id = 'com.arm.ArmTelemetrySpec';
  static readonly description = 'Arm telemetry specification plugin';
  private static manager: ArmTelemetryManager;

  static onActivate(app: App): void {
    ArmTelemetrySpecPlugin.manager = new ArmTelemetryManagerImpl(app);
  }

  async onTraceLoad(_trace: Trace): Promise<void> {}

  static getManager(): ArmTelemetryManager {
    return ArmTelemetrySpecPlugin.manager;
  }
}
