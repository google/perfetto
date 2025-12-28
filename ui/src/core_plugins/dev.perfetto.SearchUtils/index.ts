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

import m from 'mithril';
import {PerfettoPlugin} from '../../public/plugin';
import {Trace} from '../../public/trace';
import {SearchBenchmarkPage} from './search_benchmark_page';
import {SearchResultsTab} from './search_results_tab';
import {featureFlags} from '../../core/feature_flags';

const BENCHMARK_PAGE_FLAG = featureFlags.register({
  id: 'searchBenchmarkPage',
  name: 'Enable the Search Benchmark page',
  description: 'A development tool for benchmarking search algorithms.',
  defaultValue: false,
});

export default class SearchBenchmarkPlugin implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.SearchUtils';
  static readonly description =
    'Provides experimental search utilities such as a search results tab and a page that helps benchmarking search techniques.';

  async onTraceLoad(trace: Trace): Promise<void> {
    // Register the Search Results tab.
    // TODO(stevegolton): This should become a standard feature rather than
    // an experimental plugin once it's received a bit more polish.
    trace.tabs.registerTab({
      uri: 'dev.perfetto.SearchResultsTab',
      isEphemeral: false,
      content: {
        getTitle: () => 'Search Results',
        render: () => m(SearchResultsTab, {trace}),
      },
    });

    // This benchmarking page is for development purposes only and should be
    // removed once dataset search is the only search algorithm available.
    if (BENCHMARK_PAGE_FLAG.get()) {
      trace.pages.registerPage({
        route: '/search_benchmark',
        render: () => m(SearchBenchmarkPage, {trace}),
      });
      trace.sidebar.addMenuItem({
        section: 'current_trace',
        text: 'Search Benchmark',
        href: '#!/search_benchmark',
        icon: 'speed',
        sortOrder: 100,
      });
    }
  }
}
