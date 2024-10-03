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

export interface TraceFileSource {
  type: 'FILE';
  file: File;
}

export interface TraceArrayBufferSource {
  type: 'ARRAY_BUFFER';
  buffer: ArrayBuffer;
  title: string;
  url?: string;
  fileName?: string;

  // |uuid| is set only when loading via ?local_cache_key=1234. When set,
  // this matches global.state.traceUuid, with the exception of the following
  // time window: When a trace T1 is loaded and the user loads another trace T2,
  // this |uuid| will be == T2, but the globals.state.traceUuid will be
  // temporarily == T1 until T2 has been loaded (consistently to what happens
  // with all other state fields).
  uuid?: string;
  // if |localOnly| is true then the trace should not be shared or downloaded.
  localOnly?: boolean;

  // The set of extra args, keyed by plugin, that can be passed when opening the
  // trace via postMessge deep-linking. See post_message_handler.ts for details.
  pluginArgs?: {[pluginId: string]: {[key: string]: unknown}};
}

export interface TraceUrlSource {
  type: 'URL';
  url: string;

  // When loading from a permalink, the permalink might supply also the app
  // state alongside the URL of the trace.
  serializedAppState?: SerializedAppState;
}

export interface TraceHttpRpcSource {
  type: 'HTTP_RPC';
}

export type TraceSource =
  | TraceFileSource
  | TraceArrayBufferSource
  | TraceUrlSource
  | TraceHttpRpcSource;
