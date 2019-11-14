// Copyright (C) 2019 The Android Open Source Project
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

import {TrackData} from '../../common/track_data';
import {CallsiteInfo} from '../../frontend/globals';

export const HEAP_PROFILE_FLAMEGRAPH_TRACK_KIND = 'HeapProfileFlamegraphTrack';
export const HeapProfileFlamegraphKey = 'heap-profile-flamegraph';

export const SPACE_MEMORY_ALLOCATED_NOT_FREED_KEY = 'space';
export const ALLOC_SPACE_MEMORY_ALLOCATED_KEY = 'alloc_space';
export const OBJECTS_ALLOCATED_NOT_FREED_KEY = 'objects';
export const OBJECTS_ALLOCATED_KEY = 'alloc_objects';

export const DEFAULT_VIEWING_OPTION = SPACE_MEMORY_ALLOCATED_NOT_FREED_KEY;

export interface Data extends TrackData {
  flamegraph: CallsiteInfo[];
  clickedCallsite?: CallsiteInfo;
  // undefined means that there is no change since previous value.
  viewingOption?: string;
}

export interface Config {
  upid: number;
  ts: number;
  isMinimized: boolean;
  expandedId: number;
  viewingOption: string;
}
