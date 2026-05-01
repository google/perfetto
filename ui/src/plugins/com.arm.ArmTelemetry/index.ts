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

import type {App} from '../../public/app';
import type {PerfettoPlugin} from '../../public/plugin';
import type {Trace} from '../../public/trace';
import m from 'mithril';
import StandardGroupsPlugin from '../dev.perfetto.StandardGroups';
import {CpuPage} from './cpu_page';
import {ARM_TELEMETRY_CPU_SPEC_SCHEMA} from './arm_telemetry_spec';
import type {ArmTelemetryCpuSpec} from './arm_telemetry_spec';
import {z} from 'zod';
import type {ArmTelemetrySpecManager} from './arm_telemetry_spec_manager';
import {ArmTelemetrySpecManagerImpl} from './arm_telemetry_spec_manager_impl';

const ARM_TELEMETRY_CPU_SPECS_SCHEMA = z.array(ARM_TELEMETRY_CPU_SPEC_SCHEMA);

export default class ArmTelemetryPlugin implements PerfettoPlugin {
  static readonly id = 'com.arm.ArmTelemetry';
  static readonly description = `
    Provides a user interface to load and visualize Arm CPU specification files.
  `;
  static readonly dependencies = [StandardGroupsPlugin];
  private static specManager: ArmTelemetrySpecManager;

  static onActivate(app: App): void {
    const cpuSpecsSetting = app.settings.register<ArmTelemetryCpuSpec[]>({
      id: `${ArmTelemetryPlugin.id}.cpuSpecs`,
      name: 'Arm telemetry CPU specs',
      description: `Arm CPU telemetry specifications.
        To edit, go to SUPPORT -> Arm cpu page.`,
      schema: ARM_TELEMETRY_CPU_SPECS_SCHEMA,
      defaultValue: [],
      requiresReload: true,
    });

    ArmTelemetryPlugin.specManager = new ArmTelemetrySpecManagerImpl(
      cpuSpecsSetting,
    );

    app.pages.registerPage({
      route: '/arm_cpu',
      render: () =>
        m(CpuPage, {
          specManager: ArmTelemetryPlugin.specManager,
        }),
    });
    app.sidebar.addMenuItem({
      section: 'support',
      text: 'Arm cpu',
      href: '#!/arm_cpu',
      icon: 'memory',
    });
  }

  async onTraceLoad(_trace: Trace): Promise<void> {}
}
