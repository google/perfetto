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

// Copyright 2025 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

import {createStore} from '../base/store';
import sourcemap from 'source-map';
import {SourceMapDownloader} from './sourcemap_downloader';
import m from 'mithril';

export interface SourceMapInfo {
  runtime_id: string;
  url: string;
  page_url: string;
  key: string;
  [key: string]: string;
}

export interface SourceMapDecodeInfo {
  type: 'success' | 'fail' | 'download_fail';
  message: string;
  state: SourceMapDecodeState;
}

export type SourceMapDecodeState =
  | 'init'
  | 'decoding'
  | 'fail'
  | 'uploading'
  | 'uploaded'
  | 'opened';

export interface SourceMapDecodePopup {
  render(): m.Children;
}

interface SourceMapData {
  key: string;
  data: string;
}

interface SourceMapData {
  key: string;
  data: string;
}

interface SourceFile {
  key: string;
  content: string;
}

interface State {
  sourceMapInfoByUrl: Map<string, SourceMapInfo>;
  sourceMapDataByUrl: Record<string, SourceMapData>;
  sourceMapConsumerByUrl: Map<string, sourcemap.SourceMapConsumer>;
  sourceFile: Record<string, SourceFile>;
  hasJSProfileTrace?: boolean;
  sourceMapDecodePages: Set<string>;
  sourceMapDecodeInfo: SourceMapDecodeInfo[];
  currentSourceFile?: string;
  sourceFileDrawerVisible?: boolean;
  sourceMapDecodeState: SourceMapDecodeState;
  sourceMapDecodedTrace?: {
    buffer: ArrayBufferLike;
    region: string;
  };
  sourceMapDecodedTraceUrl?: string;
  sourceMapDownloader?: SourceMapDownloader;
  sourceMapDecodePopup?: SourceMapDecodePopup;
}

const emptyState: State = {
  sourceMapInfoByUrl: new Map(),
  sourceMapDataByUrl: {},
  sourceMapConsumerByUrl: new Map(),
  sourceFile: {},
  hasJSProfileTrace: false,
  sourceMapDecodeInfo: [],
  sourceMapDecodeState: 'init',
  sourceMapDecodePages: new Set(),
  sourceMapDecodedTrace: undefined,
  sourceMapDecodedTraceUrl: undefined,
  currentSourceFile: undefined,
  sourceFileDrawerVisible: false,
  sourceMapDecodePopup: undefined,
};

export const sourceMapState = createStore<State>(emptyState);

export function clearSourceMapState() {
  sourceMapState.edit((draft) => {
    Object.assign(draft, emptyState);
  });
}
