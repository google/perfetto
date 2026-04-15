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

import {z} from 'zod';

// Management of Arm Telemetry specification. The specification describes
// PMU events available on a specific Arm CPU. These events are used by
// metrics used to understand the hardware performance.
// Additionaly, the specification includes groups and a capture tree to
// structure the performance analysis.

// Description of an event
const ARM_TELEMETRY_PMU_EVENT_SCHEMA = z.object({
  code: z.string(), // Note: "0x.."
  title: z.string(),
  description: z.string(),
});

// Description of a metric
const ARM_TELEMETRY_PMU_METRIC_SCHEMA = z.object({
  title: z.string(),
  formula: z.string(),
  description: z.string(),
  units: z.string(),
  // Note would be constrained to Array<keyof ArmPmuDesc["events"]>
  events: z.array(z.string()),
});
export type ArmTelemetryPmuMetric = z.infer<
  typeof ARM_TELEMETRY_PMU_METRIC_SCHEMA
>;

// Description of event groups
const ARM_TELEMETRY_PMU_EVENT_GROUP_SCHEMA = z.object({
  title: z.string(),
  description: z.string(),
  // Note would be constrained to Array<keyof ArmPmuDesc["events"]>
  events: z.array(z.string()),
});

// Description of metric groups
const ARM_TELEMETRY_PMU_METRIC_GROUP_SCHEMA = z.object({
  title: z.string(),
  description: z.string(),
  // Note would be constrained to Array<keyof ArmPmuDesc["metrics"]>
  metrics: z.array(z.string()),
});

// Complete Arm telemetry specification description
export const ARM_TELEMETRY_CPU_SPEC_SCHEMA = z.object({
  product_configuration: z.object({
    product_name: z.string(),
    part_num: z.string(), // Note: "0x.."
    major_revision: z.coerce.number(),
    minor_revision: z.coerce.number(),
    implementer: z.string(), // Note: "0x.."
    architecture: z.string(),
    pmu_architecture: z.string(),
    num_slots: z.number(),
  }),
  events: z.record(z.string(), ARM_TELEMETRY_PMU_EVENT_SCHEMA),
  metrics: z.record(z.string(), ARM_TELEMETRY_PMU_METRIC_SCHEMA),
  groups: z.object({
    function: z.record(z.string(), ARM_TELEMETRY_PMU_EVENT_GROUP_SCHEMA),
    metrics: z.record(z.string(), ARM_TELEMETRY_PMU_METRIC_GROUP_SCHEMA),
  }),
  methodologies: z.object({
    topdown_methodology: z.object({
      decision_tree: z.object({
        // Note would be constrained to Array<keyof ArmPmuDesc["metrics"]>
        root_nodes: z.array(z.string()),
        metrics: z.array(
          z.object({
            name: z.string(),
            // Note would be constrained to
            // Array<keyof ArmPmuDesc["groups"]["metrics"]>
            next_items: z.array(z.string()),
          }),
        ),
      }),
    }),
  }),
});

export type ArmTelemetryCpuSpec = z.infer<typeof ARM_TELEMETRY_CPU_SPEC_SCHEMA>;

// Extract the CPUID of a telemetry specification.
// The CPUID is a unique identifier used to identify the CPU type.
export function getCpuId(desc: ArmTelemetryCpuSpec): string {
  return (
    desc.product_configuration.implementer +
    Number(desc.product_configuration.part_num).toString(16)
  );
}

export type CpuInfoManagerChangeCallback = (
  change: 'ADD' | 'REMOVE' | 'UPDATE',
  desc: ArmTelemetryCpuSpec,
) => void;

// A CpuInfoManager is used to manage Arm telemetry CPU descriptions loaded in the application.
// It can be used to parse, add, update and retrieve specifications.
export interface CpuInfoManager {
  // From a string parse an Arm Telemetry CPU specification
  parse(data: string): ArmTelemetryCpuSpec | undefined;

  // Add a new specification. This specification can be retrieved using getCpuDesc
  // and replaced using the update method.
  // This function will fail if a specification with the same cpuid is already registered.
  add(desc: ArmTelemetryCpuSpec): void;

  // Update an existing specification (identified by its CPUID) with new content.
  update(desc: ArmTelemetryCpuSpec): void;

  // Register a callback called when content is updated in the CpuInfoManager.
  addOnChangeCallback(callback: CpuInfoManagerChangeCallback): Disposable;

  // Return the CPUID of specification registered
  registeredCpuids(): string[];

  // Retrieve a specification from a cpuid.
  getCpuDesc(cpuid: string): ArmTelemetryCpuSpec;
}
