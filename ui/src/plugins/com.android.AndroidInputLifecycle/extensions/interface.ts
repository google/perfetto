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

import type {duration, time} from '../../../base/time';
import type {Trace} from '../../../public/trace';

export interface NavTarget {
  id: number;
  trackUri: string;
  ts: time;
  dur: duration;
  depth: number;
}

/**
 * Defines a single processing step (stage) in the input lifecycle.
 * Maps to a column in the plugin UI and specifies how to extract
 * its timestamps and navigate to its timeline slice.
 */
export interface StageDefinition {
  readonly key: string;
  readonly headerName: string;
  readonly sequenceNumber: number;

  // Mapping to SQL columns in the query result
  readonly idField: string;
  readonly trackField: string;
  readonly tsField: string;

  // The raw duration field representing execution time:
  readonly durField: string;
}

export interface SqlJoinSpec {
  readonly tableName: string;
  readonly tableAlias?: string;
  readonly joinOn: string;
}

export interface CellData {
  readonly dur: duration | null;
  readonly nav?: NavTarget;
}

/**
 * Interface for registering input lifecycle plugin extensions.
 *
 * Extensions allow custom or device-specific input pipelines (e.g. hardware driver
 * touch events or vendor-specific frameworks) to integrate their lifecycle stages
 * into the main plugin timeline and grid view.
 */
export interface InputLifecycleExtension {
  readonly id: string;
  readonly requiredModules?: string[];
  isEligible(trace: Trace): Promise<boolean>;

  // Returns the stages this extension introduces
  getStages(): StageDefinition[];

  // Returns instructions on how to join the extension table
  getSqlJoinSpec(): SqlJoinSpec;

  resolveInputId?(trace: Trace, sliceId: number): Promise<string | undefined>;
}
