// Copyright (C) 2024 The Android Open Source Project
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

export const FTRACE_RAW_TRACK_KIND = 'FtraceRawTrack';

export const FTRACE_FILTER_SCHEMA = z.object({
  // We use an exclude list rather than include list for filtering events, as we
  // want to include all events by default but we won't know what names are
  // present initially.
  excludeList: z.array(z.string()),
  // Inclusion list of ucpu ids shown in the standalone ftrace tab. Undefined
  // means all CPUs are shown.
  visibleCpus: z.array(z.number()).optional(),
});

export type FtraceFilter = z.infer<typeof FTRACE_FILTER_SCHEMA>;

// Backwards-compatibility schema for older permalinks that stored
// { version: 2, filter: { excludeList: ... } }
const LEGACY_FTRACE_SCHEMA = z
  .object({
    version: z.number().optional(),
    filter: FTRACE_FILTER_SCHEMA,
  })
  .transform((legacy) => legacy.filter);

export const FTRACE_STATE_SCHEMA = z.union([
  FTRACE_FILTER_SCHEMA,
  LEGACY_FTRACE_SCHEMA,
]);

export interface FtraceStat {
  name: string;
  count: number;
}
