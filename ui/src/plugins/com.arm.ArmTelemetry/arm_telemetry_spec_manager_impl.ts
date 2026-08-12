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

import type {Setting} from '../../public/settings';
import {ARM_TELEMETRY_CPU_SPEC_SCHEMA, getCpuId} from './arm_telemetry_spec';
import type {ArmTelemetryCpuSpec} from './arm_telemetry_spec';
import type {
  ArmTelemetrySpecManager,
  ArmTelemetrySpecChangeCallback,
} from './arm_telemetry_spec_manager';

export function validateArmTelemetrySpec(
  cpuDesc: unknown,
): ArmTelemetryCpuSpec {
  const result = ARM_TELEMETRY_CPU_SPEC_SCHEMA.parse(cpuDesc);

  const eventNames = Object.keys(result.events);
  const metricNames = Object.keys(result.metrics);

  for (const [name, metric] of Object.entries(result.metrics)) {
    if (!metric.events.every((eventName) => eventNames.includes(eventName))) {
      throw new Error(`Non existent event listed in metric ${name}`);
    }
  }

  for (const [name, group] of Object.entries(result.groups.function)) {
    if (!group.events.every((eventName) => eventNames.includes(eventName))) {
      throw new Error(`Non existent event listed in function group ${name}`);
    }
  }

  for (const [name, group] of Object.entries(result.groups.metrics)) {
    if (
      !group.metrics.every((metricName) => metricNames.includes(metricName))
    ) {
      throw new Error(`Non existent metric listed in function group ${name}`);
    }
  }

  const rootNodes =
    result.methodologies.topdown_methodology.decision_tree.root_nodes;
  if (!rootNodes.every((metricName) => metricNames.includes(metricName))) {
    throw new Error(`Non existent metric listed at top down methodology root`);
  }

  const metricAndMetricGroupNames = [
    ...Object.keys(result.groups.metrics),
    ...Object.keys(result.metrics),
  ];
  const topDownMetrics =
    result.methodologies.topdown_methodology.decision_tree.metrics;
  for (const metric of topDownMetrics) {
    if (
      !metric.next_items.every((groupName) =>
        metricAndMetricGroupNames.includes(groupName),
      )
    ) {
      throw new Error(
        `Non existent metric group ${metric.name} listed in top down methodology tree`,
      );
    }
  }

  return result;
}

export function parseArmTelemetrySpec(
  data: string,
): ArmTelemetryCpuSpec | undefined {
  try {
    const json = JSON.parse(data);
    return validateArmTelemetrySpec(json);
  } catch {
    return undefined;
  }
}

export class ArmTelemetrySpecManagerImpl implements ArmTelemetrySpecManager {
  constructor(
    private readonly cpuSpecsSetting: Setting<ArmTelemetryCpuSpec[]>,
  ) {}
  private readonly changeCallbacks = new Set<ArmTelemetrySpecChangeCallback>();

  add(desc: ArmTelemetryCpuSpec): void {
    this.cpuSpecsSetting.set([...this.cpuSpecsSetting.get(), desc]);
    this.changeCallbacks.forEach((cb) => cb({kind: 'ADD', desc}));
  }

  update(desc: ArmTelemetryCpuSpec): void {
    const cpuid = getCpuId(desc);
    const specs = [...this.cpuSpecsSetting.get()];
    const index = specs.findIndex((spec) => getCpuId(spec) === cpuid);
    if (index === -1) {
      this.add(desc);
    } else {
      specs[index] = desc;
      this.cpuSpecsSetting.set(specs);
      this.changeCallbacks.forEach((cb) => cb({kind: 'UPDATE', desc}));
    }
  }

  clear(): void {
    this.cpuSpecsSetting.set([]);
    this.changeCallbacks.forEach((cb) => cb({kind: 'CLEAR'}));
  }

  hasSpecs(): boolean {
    return this.cpuSpecsSetting.get().length > 0;
  }

  registeredCpuids(): string[] {
    return this.cpuSpecsSetting.get().map((spec) => getCpuId(spec));
  }

  getCpuDesc(cpuid: string): ArmTelemetryCpuSpec {
    const desc = this.cpuSpecsSetting
      .get()
      .find((spec) => getCpuId(spec) === cpuid);
    if (desc === undefined) {
      throw new Error(`No Arm telemetry spec registered for CPU ${cpuid}`);
    }
    return desc;
  }

  addOnChangeCallback(callback: ArmTelemetrySpecChangeCallback): Disposable {
    this.changeCallbacks.add(callback);
    return {
      [Symbol.dispose]: () => {
        this.changeCallbacks.delete(callback);
      },
    };
  }
}
