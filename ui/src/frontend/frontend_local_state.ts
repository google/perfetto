// Copyright (C) 2018 The Android Open Source Project
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

import {TimeScale} from './time_scale';

/**
 * State that is shared between several frontend components, but not the
 * controller. This state is updated at 60fps.
 */
export interface FrontendLocalState {
  timeScale: TimeScale;
  visibleWindowMs: {start: number; end: number;};
}

export function createEmptyFrontendState(): FrontendLocalState {
  return {
    timeScale: new TimeScale([0, 0], [0, 0]),
    visibleWindowMs: {start: 0, end: 1000000},
  };
}