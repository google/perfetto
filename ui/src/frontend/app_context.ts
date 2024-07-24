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

import {CurrentSearchResults} from '../common/search_data';
import {State} from '../common/state';
import {Store} from '../public';
import {Timeline} from './timeline';
import {TraceContext} from './trace_context';

export interface AppContext {
  readonly store: Store<State>;
  readonly state: State;
  readonly traceContext: TraceContext;

  // TODO(stevegolton): This could probably be moved into TraceContext.
  readonly timeline: Timeline;

  // TODO(stevegolton): Move this into the search subsystem when it exists.
  readonly currentSearchResults: CurrentSearchResults;
}
