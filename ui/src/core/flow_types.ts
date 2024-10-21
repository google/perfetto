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

import {time, duration} from '../base/time';
import {SliceSqlId} from '../trace_processor/sql_utils/core_types';

export interface Flow {
  id: number;

  begin: FlowPoint;
  end: FlowPoint;
  dur: duration;

  // Whether this flow connects a slice with its descendant.
  flowToDescendant: boolean;

  category?: string;
  name?: string;
}

export interface FlowPoint {
  trackId: number;

  sliceName: string;
  sliceCategory: string;
  sliceId: SliceSqlId;
  sliceStartTs: time;
  sliceEndTs: time;
  // Thread and process info. Only set in sliceSelected not in areaSelected as
  // the latter doesn't display per-flow info and it'd be a waste to join
  // additional tables for undisplayed info in that case. Nothing precludes
  // adding this in a future iteration however.
  threadName: string;
  processName: string;

  depth: number;

  // TODO(altimin): Ideally we should have a generic mechanism for allowing to
  // customise the name here, but for now we are hardcording a few
  // Chrome-specific bits in the query here.
  sliceChromeCustomName?: string;
}

export type FlowDirection = 'Forward' | 'Backward';
