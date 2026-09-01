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

import {z} from 'zod';

const ARM_TELEMETRY_PMU_EVENT_SCHEMA = z.object({
  code: z.string(),
  title: z.string(),
  description: z.string(),
});

const ARM_TELEMETRY_PMU_METRIC_SCHEMA = z.object({
  title: z.string(),
  formula: z.string(),
  description: z.string(),
  units: z.string(),
  events: z.array(z.string()),
});

export type ArmTelemetryPmuMetric = z.infer<
  typeof ARM_TELEMETRY_PMU_METRIC_SCHEMA
>;

const ARM_TELEMETRY_PMU_EVENT_GROUP_SCHEMA = z.object({
  title: z.string(),
  description: z.string(),
  events: z.array(z.string()),
});

const ARM_TELEMETRY_PMU_METRIC_GROUP_SCHEMA = z.object({
  title: z.string(),
  description: z.string(),
  metrics: z.array(z.string()),
});

export const ARM_TELEMETRY_CPU_SPEC_SCHEMA = z.object({
  product_configuration: z.object({
    product_name: z.string(),
    part_num: z.string(),
    major_revision: z.coerce.number(),
    minor_revision: z.coerce.number(),
    implementer: z.string(),
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
        root_nodes: z.array(z.string()),
        metrics: z.array(
          z.object({
            name: z.string(),
            next_items: z.array(z.string()),
          }),
        ),
      }),
    }),
  }),
});

export type ArmTelemetryCpuSpec = z.infer<typeof ARM_TELEMETRY_CPU_SPEC_SCHEMA>;

export function getCpuId(desc: ArmTelemetryCpuSpec): string {
  return (
    desc.product_configuration.implementer +
    Number(desc.product_configuration.part_num).toString(16)
  );
}
