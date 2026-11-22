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
import {MemscopeHomePage} from './home_page';
import {
  MEMSCOPE_AGENT_PLUGIN_ID,
  MemscopeAgentPlugin,
} from './memscope_agent';
import {
  lastMemoryExplorerHref,
  traceHasMemoryExplorer,
} from '../../../ui/src/plugins/dev.perfetto.Memscope/memory_explorer/route';
import {
  hasPendingMemoryLoadOrError,
  isMemoryExplorerLoading,
  memoryLoadingHref,
} from '../../../ui/src/plugins/dev.perfetto.Memscope/memory_explorer/loading_state';

const PLUGINS = [
  'com.android.AndroidDmabuf',
  'com.android.HeapDumpExplorer',
  'dev.perfetto.CoreCommands',
  'dev.perfetto.FlowEvents',
  'dev.perfetto.GlobalGroups',
  'dev.perfetto.HeapProfile',
  'dev.perfetto.Llm',
  'dev.perfetto.LlmProtocolChromePrompt',
  'dev.perfetto.LlmProtocolGemini',
  'dev.perfetto.LlmProtocolOpenAi',
  'dev.perfetto.Memscope',
  'dev.perfetto.Notes',
  'dev.perfetto.Process',
  'dev.perfetto.ProcessThreadGroups',
  'dev.perfetto.QueryPage',
  'dev.perfetto.RecordTraceV2',
  'dev.perfetto.Sched',
  'dev.perfetto.SettingsPage',
  'dev.perfetto.Smaps',
  'dev.perfetto.SqlModules',
  'dev.perfetto.StandardGroups',
  'dev.perfetto.Thread',
  'dev.perfetto.Timeline',
  'dev.perfetto.TraceProcessorTrack',
  'dev.perfetto.TrackUtils',
];

export class MemscopeEmbedder implements Embedder {
  readonly productId = 'memscope';
  readonly appTitle = 'Memscope';
  readonly analyticsId = undefined;
  readonly extensionServer = undefined;
  readonly brandingBadge = undefined;
  readonly defaultPlugins = [...PLUGINS, MEMSCOPE_AGENT_PLUGIN_ID];
  readonly pluginAllowlist = PLUGINS;
  readonly extraPlugins = [MemscopeAgentPlugin];
  readonly renderHomePage = () => m(MemscopeHomePage);
  readonly traceLoadingRoute = '/memory';
  readonly navigation: Navigation = {
    kind: 'tabs',
    brand: {text: 'Memscope', icon: 'memory'},
    tabs: (app) => {
      const loadingMemory = isMemoryExplorerLoading(app);
      return [
        {label: 'Live', page: '/memscope', icon: 'sensors'},
        {
          label: 'Memory Explorer',
          page: '/memory',
          icon: 'memory',
          loading: loadingMemory,
          href: () =>
            loadingMemory
              ? memoryLoadingHref()
              : lastMemoryExplorerHref(app.trace?.traceInfo.uuid),
          isVisible: () =>
            traceHasMemoryExplorer(app.trace?.traceInfo.uuid) ||
            hasPendingMemoryLoadOrError(app),
        },
        {
          label: 'Timeline',
          page: '/viewer',
          icon: 'timeline',
          isVisible: () => app.trace !== undefined && !app.isLoadingTrace,
        },
      ];
    },
  };
}
