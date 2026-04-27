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

import {Registry} from '../../base/registry';
import {exists} from '../../base/utils';
import {App} from '../../public/app';
import {
  ARM_TELEMETRY_CPU_SPEC_SCHEMA,
  ArmTelemetryCpuSpec,
  getCpuId,
} from './arm_telemetry_spec';
import {
  ArmTelemetrySpecChangeCallback,
  ArmTelemetryManager,
} from './arm_telemetry_spec_manager';

function validateArmTelemetrySpec(cpuDesc: unknown): ArmTelemetryCpuSpec {
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

class CpuRegistry extends Registry<ArmTelemetryCpuSpec> {
  constructor() {
    super((cpu) => getCpuId(cpu));
  }
}

export class ArmTelemetryManagerImpl implements ArmTelemetryManager {
  constructor(private readonly app: App) {}

  private readonly cpuRegistry = new CpuRegistry();
  private readonly changeCallbacks = new Set<ArmTelemetrySpecChangeCallback>();
  private readonly registryEntries = new Map<string, Disposable>();

  parse(data: string): ArmTelemetryCpuSpec | undefined {
    try {
      const json = JSON.parse(data);
      return validateArmTelemetrySpec(json);
    } catch {
      return undefined;
    }
  }

  add(desc: ArmTelemetryCpuSpec): void {
    const entry = this.cpuRegistry.register(desc);
    this.registryEntries.set(getCpuId(desc), entry);
    this.changeCallbacks.forEach((cb) => cb('ADD', desc));
    this.app.raf.scheduleFullRedraw();
  }

  update(desc: ArmTelemetryCpuSpec): void {
    let entry = this.registryEntries.get(getCpuId(desc));
    if (exists(entry)) {
      entry[Symbol.dispose]();
      entry = this.cpuRegistry.register(desc);
      this.registryEntries.set(getCpuId(desc), entry);
      this.changeCallbacks.forEach((cb) => cb('UPDATE', desc));
      this.app.raf.scheduleFullRedraw();
      return;
    }
    this.add(desc);
  }

  addOnChangeCallback(callback: ArmTelemetrySpecChangeCallback): Disposable {
    this.changeCallbacks.add(callback);
    return {
      [Symbol.dispose]: () => {
        this.changeCallbacks.delete(callback);
      },
    };
  }

  registeredCpuids(): string[] {
    return [...this.registryEntries.keys()];
  }

  getCpuDesc(cpuid: string): ArmTelemetryCpuSpec {
    return this.cpuRegistry.get(cpuid);
  }
}
