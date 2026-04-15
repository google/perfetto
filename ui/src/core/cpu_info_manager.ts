// Copyright (C) 2025 The Android Open Source Project
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

import {exists} from '../base/utils';
import {raf} from './raf_scheduler';
import {Registry} from '../base/registry';
import {
  ARM_TELEMETRY_CPU_SPEC_SCHEMA,
  ArmTelemetryCpuSpec,
  CpuInfoManager,
  CpuInfoManagerChangeCallback,
  getCpuId,
} from '../public/cpu_info';

// Arm PMU data is coming from the telemetry solution repository:
// https://gitlab.arm.com/telemetry-solution/telemetry-solution/-/tree/main/data

// Validator of telemetry solution cpu description.
// First it validates the data follow the schema
// Then it validates data consistency
function validateArmTelemetrySpec(cpuDesc: unknown): ArmTelemetryCpuSpec {
  const result = ARM_TELEMETRY_CPU_SPEC_SCHEMA.parse(cpuDesc);

  const eventNames = Object.keys(result.events);
  const metricNames = Object.keys(result.metrics);

  // validate events listed in each metrics
  for (const [name, metric] of Object.entries(result.metrics)) {
    if (!metric.events.every((e) => eventNames.includes(e))) {
      throw new Error(`Non existent event listed in metric ${name}`);
    }
  }

  // validate events listed in each function group
  for (const [name, group] of Object.entries(result.groups.function)) {
    if (!group.events.every((e) => eventNames.includes(e))) {
      throw new Error(`Non existent event listed in function group ${name}`);
    }
  }

  // validate metrics listed in each metrics group
  for (const [name, group] of Object.entries(result.groups.metrics)) {
    if (!group.metrics.every((m) => metricNames.includes(m))) {
      throw new Error(`Non existent metric listed in function group ${name}`);
    }
  }

  // validate root nodes as metrics
  const rootNodes =
    result.methodologies.topdown_methodology.decision_tree.root_nodes;
  if (!rootNodes.every((m) => metricNames.includes(m))) {
    throw new Error(`Non existent metric listed at top down methodology root`);
  }

  // Validate leafs as metric or metric group
  const metricAndMetricGroupNames = [
    ...Object.keys(result.groups.metrics),
    ...Object.keys(result.metrics),
  ];
  const topDownMetrics =
    result.methodologies.topdown_methodology.decision_tree.metrics;
  for (const m of topDownMetrics) {
    if (!m.next_items.every((g) => metricAndMetricGroupNames.includes(g))) {
      throw new Error(
        `Non existent metric group ${m.name} listed in top down methodology tree`,
      );
    }
  }

  return result;
}

// Registry containing cpu descriptions.
// Available descriptions can be retrieved using the CpuInfoManager in App
class CpuRegistry extends Registry<ArmTelemetryCpuSpec> {
  constructor() {
    super((cpu) => getCpuId(cpu));
  }
}

// The cpu manager parse and manages cpu descriptions
export class CpuInfoManagerImpl implements CpuInfoManager {
  private cpuRegistry: CpuRegistry = new CpuRegistry();
  private changeCallbacks: Set<CpuInfoManagerChangeCallback> = new Set();
  private registryEntries: Map<string, Disposable> = new Map();

  parse(data: string): ArmTelemetryCpuSpec | undefined {
    try {
      const json = JSON.parse(data);
      return validateArmTelemetrySpec(json);
    } catch (e) {
      return undefined;
    }
  }

  add(desc: ArmTelemetryCpuSpec): void {
    const entry = this.cpuRegistry.register(desc);
    this.registryEntries.set(getCpuId(desc), entry);
    this.changeCallbacks.forEach((cb) => {
      cb('ADD', desc);
    });
    raf.scheduleFullRedraw();
  }

  update(desc: ArmTelemetryCpuSpec): void {
    let entry = this.registryEntries.get(getCpuId(desc));
    if (exists(entry)) {
      entry[Symbol.dispose]();
      entry = this.cpuRegistry.register(desc);
      this.registryEntries.set(getCpuId(desc), entry);
      this.changeCallbacks.forEach((cb) => {
        cb('UPDATE', desc);
      });
      raf.scheduleFullRedraw();
    } else {
      this.add(desc);
    }
  }

  addOnChangeCallback(callback: CpuInfoManagerChangeCallback): Disposable {
    const changeCallbacks = this.changeCallbacks;
    changeCallbacks.add(callback);
    return {
      [Symbol.dispose](): void {
        changeCallbacks.delete(callback);
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
