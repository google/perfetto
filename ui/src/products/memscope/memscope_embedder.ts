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

import m from 'mithril';
import type {Embedder} from '../../core/embedder/embedder';
import {MemscopeHomePage} from './home_page';

const PLUGINS = [
  'com.android.HeapDumpExplorer',
  'dev.perfetto.HeapProfile',
  'dev.perfetto.Llm',
  'dev.perfetto.LlmProtocolChromePrompt',
  'dev.perfetto.LlmProtocolGemini',
  'dev.perfetto.LlmProtocolOpenAi',
  'dev.perfetto.Memscope',
  'dev.perfetto.Process',
  'dev.perfetto.ProcessThreadGroups',
  'dev.perfetto.RecordTraceV2',
  'dev.perfetto.Sched',
  'dev.perfetto.Smaps',
  'dev.perfetto.StandardGroups',
  'dev.perfetto.Thread',
  'dev.perfetto.TraceProcessorTrack',
];

export class MemscopeEmbedder implements Embedder {
  readonly productId = 'memscope';
  readonly appTitle = 'Memscope';
  readonly analyticsId = undefined;
  readonly extensionServer = undefined;
  readonly brandingBadge = {text: 'Memscope', icon: 'memory'};
  readonly defaultPlugins = PLUGINS;
  readonly pluginAllowlist = PLUGINS;
  readonly renderHomePage = () => m(MemscopeHomePage);
}
