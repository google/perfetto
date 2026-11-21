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
import type {
  Embedder,
  Navigation,
} from '../../../ui/src/core/embedder/embedder';
import {SysmemHomePage} from './home_page';
import {SYSMEM_TRACE_ROUTE} from '../../../ui/src/plugins/dev.perfetto.Sysmem/trace_dashboard';

// Sysmem builds on Memscope's device connection and live sampling, so it
// needs Memscope and its dependencies too.
const PLUGINS = [
  'com.android.HeapDumpExplorer',
  'dev.perfetto.HeapProfile',
  'dev.perfetto.Memscope',
  'dev.perfetto.Process',
  'dev.perfetto.ProcessThreadGroups',
  'dev.perfetto.RecordTraceV2',
  'dev.perfetto.Sysmem',
];

export class SysmemEmbedder implements Embedder {
  readonly productId = 'sysmem';
  readonly appTitle = 'Sysmem';
  readonly analyticsId = undefined;
  readonly extensionServer = undefined;
  readonly brandingBadge = undefined;
  readonly defaultPlugins = PLUGINS;
  readonly pluginAllowlist = PLUGINS;
  readonly renderHomePage = () => m(SysmemHomePage);
  readonly navigation: Navigation = {
    kind: 'tabs',
    brand: {text: 'Sysmem', icon: 'developer_board'},
    tabs: [
      {label: 'Live', page: '/sysmem'},
      {
        label: 'Trace',
        page: SYSMEM_TRACE_ROUTE,
        isVisible: (app) => app.trace !== undefined,
      },
    ],
  };
}
