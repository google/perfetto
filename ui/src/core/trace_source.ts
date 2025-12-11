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

import {SerializedAppState} from './state_serialization_schema';
import {TraceStream} from '../public/stream';

interface CommonTraceProps {
  serializedAppState?: SerializedAppState;
}

export type TraceSource = CommonTraceProps &
  (
    | TraceFileSource
    | TraceArrayBufferSource
    | TraceUrlSource
    | TraceHttpRpcSource
    | TraceMultipleFilesSource
    | TraceStreamSource
  );

export interface TraceFileSource {
  readonly type: 'FILE';
  readonly file: File;
}

export interface TraceMultipleFilesSource {
  readonly type: 'MULTIPLE_FILES';
  readonly files: ReadonlyArray<File>;
}

export interface TraceUrlSource {
  readonly type: 'URL';
  readonly url: string;
}

export interface TraceStreamSource {
  type: 'STREAM';
  stream: TraceStream;
}

export interface TraceHttpRpcSource {
  readonly type: 'HTTP_RPC';
}

export interface TraceArrayBufferSource {
  readonly type: 'ARRAY_BUFFER';
  readonly buffer: ArrayBuffer;
  readonly title: string;
  readonly fileName?: string;
  readonly url?: string;

  // |uuid| is set only when loading via ?local_cache_key=1234. When set,
  // this matches global.state.traceUuid, with the exception of the following
  // time window: When a trace T1 is loaded and the user loads another trace T2,
  // this |uuid| will be == T2, but the globals.state.traceUuid will be
  // temporarily == T1 until T2 has been loaded (consistently to what happens
  // with all other state fields).
  readonly uuid?: string;

  // if |localOnly| is true then the trace should not be shared or downloaded.
  readonly localOnly?: boolean;

  // Allows to pass extra arguments to plugins. This can be read by plugins
  // onTraceLoad() and can be used to trigger plugin-specific-behaviours (e.g.
  // allow dashboards like APC to pass extra data to materialize onto tracks).
  // The format is the following:
  // pluginArgs: {
  //   'dev.perfetto.PluginFoo': { 'key1': 'value1', 'key2': 1234 }
  //   'dev.perfetto.PluginBar': { 'key3': '...', 'key4': ... }
  // }
  readonly pluginArgs?: {[pluginId: string]: {[key: string]: unknown}};
}
