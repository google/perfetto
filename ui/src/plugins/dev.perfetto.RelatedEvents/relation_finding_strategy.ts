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

import {Dataset} from '../../trace_processor/dataset';
import {LONG, NUM, STR} from '../../trace_processor/query_result';
import {Trace} from '../../public/trace';
import {duration, time} from '../../base/time';

// ---------------------------------------------------
// Strategy interface
// ---------------------------------------------------

export const RELATED_EVENT_SCHEMA = {
  id: LONG,
  name: STR,
  ts: LONG,
  dur: LONG,
  track_id: LONG,
};

export const RELATION_SCHEMA = {
  ...RELATED_EVENT_SCHEMA,
  depth: NUM,
};

export type EventContext = {
  sliceId: number;
  name: string;
  args: Map<string, string>;
  ts: time;
  dur: duration;
};

export interface RelationRule {
  /**
   * Returns a list of SourceDataset objects if this rule applies to the current event.
   * Returns empty array if not applicable.
   */
  getRelatedEventsAsDataset(ctx: EventContext): Dataset[];
}
export interface RelationFindingStrategy {
  findRelatedEvents(trace: Trace): Promise<Dataset | undefined>;
}
