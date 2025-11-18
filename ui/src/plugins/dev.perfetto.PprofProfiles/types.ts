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

import {QueryFlamegraphMetric} from '../../components/query_flamegraph';
import {FLAMEGRAPH_STATE_SCHEMA} from '../../widgets/flamegraph';
import {z} from 'zod';

export const PPROF_PAGE_STATE_SCHEMA = z.object({
  flamegraphState: FLAMEGRAPH_STATE_SCHEMA.optional(),
  selectedProfileId: z.string().optional(),
});

export type PprofPageState = z.infer<typeof PPROF_PAGE_STATE_SCHEMA>;

export interface PprofProfile {
  readonly id: string;
  readonly displayName: string;
  readonly metrics: ReadonlyArray<QueryFlamegraphMetric>;
}
