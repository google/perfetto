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

import type {TreeExplorerQueryMetric} from '../../components/tree_explorer_fetcher';
import {TREE_EXPLORER_STATE_SCHEMA} from '../../widgets/tree_explorer';
import type {TreeExplorerProfile} from '../../widgets/tree_explorer_merge';
import {z} from 'zod';

export const AGGREGATE_PROFILES_PAGE_STATE_SCHEMA = z.object({
  // Also holds which profile is shown, or that all of them are merged (see
  // TreeExplorerState.merge).
  flamegraphState: TREE_EXPLORER_STATE_SCHEMA.optional(),
});

export type AggregateProfilesPageState = z.infer<
  typeof AGGREGATE_PROFILES_PAGE_STATE_SCHEMA
>;

// One profile of the trace, e.g. one file of a pprof archive, keyed and
// labeled by its scope.
export interface AggregateProfile extends TreeExplorerProfile {
  readonly metrics: ReadonlyArray<TreeExplorerQueryMetric>;
}
