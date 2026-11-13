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
import {ProfileType} from './common';

export const HEAP_PROFILE_METRIC_SCHEMA = z.enum([
  'unreleased_size',
  'unreleased_count',
  'total_size',
  'total_count',
]);

export type HeapProfileMetric = z.infer<typeof HEAP_PROFILE_METRIC_SCHEMA>;

export const HEAP_PROFILE_DEEPLINK_ARGS_SCHEMA = z.object({
  process: z.string().nonempty().optional(),
  type: z
    .string()
    .nonempty()
    .transform((t) => (t.startsWith('heap_profile:') ? t : `heap_profile:${t}`))
    .optional(),
  metric: HEAP_PROFILE_METRIC_SCHEMA.optional(),
});

export type HeapProfileDeeplinkArgs = z.infer<
  typeof HEAP_PROFILE_DEEPLINK_ARGS_SCHEMA
>;

export function resolveFlamegraphMetricName(
  profileType: ProfileType,
  metric: HeapProfileMetric,
): string | undefined {
  switch (profileType) {
    case ProfileType.NATIVE_HEAP_PROFILE:
      switch (metric) {
        case 'unreleased_size':
          return 'Unreleased Malloc Size';
        case 'unreleased_count':
          return 'Unreleased Malloc Count';
        case 'total_size':
          return 'Total Malloc Size';
        case 'total_count':
          return 'Total Malloc Count';
      }
      break;
    case ProfileType.GENERIC_HEAP_PROFILE:
      switch (metric) {
        case 'unreleased_size':
          return 'Unreleased Size';
        case 'unreleased_count':
          return 'Unreleased Count';
        case 'total_size':
          return 'Total Size';
        case 'total_count':
          return 'Total Count';
      }
      break;
    case ProfileType.JAVA_HEAP_SAMPLES:
      switch (metric) {
        case 'total_size':
          return 'Total Allocation Size';
        case 'total_count':
          return 'Total Allocation Count';
        default:
          return undefined;
      }
    default:
      return undefined;
  }
}

export function parseHeapProfileDeeplinkArgs(
  args: Record<string, unknown>,
): HeapProfileDeeplinkArgs | undefined {
  const res = HEAP_PROFILE_DEEPLINK_ARGS_SCHEMA.safeParse(args);
  if (!res.success) {
    return undefined;
  }
  const {process, type, metric} = res.data;
  if (process === undefined && type === undefined && metric === undefined) {
    return undefined;
  }
  return res.data;
}
