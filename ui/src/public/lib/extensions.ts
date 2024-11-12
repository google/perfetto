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

import {type addDebugSliceTrack} from '../debug_tracks';
import {type addDebugCounterTrack} from './tracks/debug_tracks';
import {type addSqlTableTab} from '../../frontend/sql_table_tab';
import {type addVisualizedArgTracks} from '../../frontend/visualized_args_tracks';
import {type addQueryResultsTab} from './query_table/query_result_tab';

// TODO(primiano & stevegolton): This injection is to break the circular
// dependency cycle that there is between various tabs and tracks.
//
// For example: DebugSliceTrack has a DebugSliceDetailsTab which shows details
// about slices, which have a context menu, which allows to create a debug track
// from it. We will break this cycle "more properly" by either:
// 1. having a registry for context menu items for slices
// 2. allowing plugins to expose API for the use of other plugins, and putting
//    these extension points there instead

export interface ExtensionApi {
  addDebugSliceTrack: typeof addDebugSliceTrack;
  addDebugCounterTrack: typeof addDebugCounterTrack;
  addSqlTableTab: typeof addSqlTableTab;
  addVisualizedArgTracks: typeof addVisualizedArgTracks;
  addQueryResultsTab: typeof addQueryResultsTab;
}

export let extensions: ExtensionApi;

export function configureExtensions(e: ExtensionApi) {
  extensions = e;
}
