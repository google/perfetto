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
import {searchTrackEvents} from '../../core/dataset_search';
import {
  compareSearchResults,
  SearchResultDifference,
  SearchResultEvent,
} from '../../core/search_result_utils';
import {executeSqlSearch} from '../../core/sql_search';
import {TrackManagerImpl} from '../../core/track_manager';
import {SearchProvider} from '../../public/search';
import {Trace} from '../../public/trace';
import {Button, ButtonVariant} from '../../widgets/button';
import {Callout} from '../../widgets/callout';
import {Intent} from '../../widgets/common';
import {Form, FormLabel} from '../../widgets/form';
import {TextInput} from '../../widgets/text_input';

interface PageAttrs {
  trace: Trace;
}

interface BenchmarkResult {
  method: string;
  duration: number;
  resultCount: number;
}

interface BenchmarkRun {
  searchTerm: string;
  iterations: number;
  timestamp: Date;
  sqlResults: BenchmarkResult[];
  datasetResults: BenchmarkResult[];
  sqlAvg: number;
  datasetAvg: number;
  differences?: SearchResultDifference;
}

export class SearchBenchmarkPage implements m.ClassComponent<PageAttrs> {
  private searchTerm: string = '';
  private iterations: number = 10;
  private running: boolean = false;
  private results: BenchmarkRun | undefined;
  private showDiffDetails: boolean = false;

  view({attrs}: m.CVnode<PageAttrs>) {
    const trace = attrs.trace as Trace;

    return m('.pf-search-benchmark-page', [
      m('h1', 'Search Performance Benchmark'),
      m(
        Form,
        m(FormLabel, {for: 'search-term'}, 'Search Term:'),
        m(TextInput, {
          id: 'search-term',
          value: this.searchTerm,
          onInput: (value: string) => {
            this.searchTerm = value;
          },
          onkeydown: (e: KeyboardEvent) => {
            if (e.key === 'Enter' && this.searchTerm && !this.running) {
              e.preventDefault();
              this.runBenchmark(trace);
            }
          },
          placeholder: 'Enter search term...',
          disabled: this.running,
        }),
        m(FormLabel, {for: 'iterations'}, 'Iterations:'),
        m(TextInput, {
          id: 'iterations',
          value: String(this.iterations),
          onInput: (value: string) => {
            const num = parseInt(value, 10);
            if (!isNaN(num) && num > 0) {
              this.iterations = num;
            }
          },
          disabled: this.running,
        }),
        m(Button, {
          label: 'Run Benchmark',
          intent: Intent.Primary,
          loading: this.running,
          variant: ButtonVariant.Filled,
          disabled: !this.searchTerm,
          onclick: (e) => {
            e.preventDefault();
            this.runBenchmark(trace);
          },
        }),
      ),
      this.results && this.renderResults(),
    ]);
  }

  private renderDifferenceCallout() {
    const diff = this.results?.differences;
    if (!diff) return null;

    const hasDifferences =
      diff.missingInResult2.length > 0 ||
      diff.missingInResult1.length > 0 ||
      diff.different.length > 0;

    if (!hasDifferences) {
      return m(
        Callout,
        {
          icon: 'check_circle',
          intent: Intent.Primary,
        },
        'Results match! Both search methods returned identical results.',
      );
    }

    return m('.pf-search-benchmark-diff', [
      m(
        Callout,
        {
          icon: 'warning',
          intent: Intent.Warning,
        },
        [
          m('div', [
            m('strong', 'Results differ between SQL and Dataset search!'),
            m(Button, {
              label: this.showDiffDetails ? 'Hide Details' : 'Show Details',
              minimal: true,
              compact: true,
              onclick: () => {
                this.showDiffDetails = !this.showDiffDetails;
              },
            }),
          ]),
          this.showDiffDetails &&
            m('.pf-diff-details', [
              diff.result1Count !== diff.result2Count &&
                m('p', [
                  `Result count mismatch: SQL found ${diff.result1Count} results, Dataset found ${diff.result2Count} results`,
                ]),
              diff.missingInResult2.length > 0 &&
                m('div', [
                  m(
                    'h4',
                    `${diff.missingInResult2.length} results in SQL but not in Dataset:`,
                  ),
                  m(
                    'ul',
                    diff.missingInResult2
                      .slice(0, 10)
                      .map((item) =>
                        m('li', [
                          `ID: ${item.id}, TS: ${item.ts}, Track: ${item.trackUri}`,
                        ]),
                      ),
                  ),
                  diff.missingInResult2.length > 10 &&
                    m('p', `...and ${diff.missingInResult2.length - 10} more`),
                ]),
              diff.missingInResult1.length > 0 &&
                m('div', [
                  m(
                    'h4',
                    `${diff.missingInResult1.length} results in Dataset but not in SQL:`,
                  ),
                  m(
                    'ul',
                    diff.missingInResult1
                      .slice(0, 10)
                      .map((item) =>
                        m('li', [
                          `ID: ${item.id}, TS: ${item.ts}, Track: ${item.trackUri}`,
                        ]),
                      ),
                  ),
                  diff.missingInResult1.length > 10 &&
                    m('p', `...and ${diff.missingInResult1.length - 10} more`),
                ]),
              diff.different.length > 0 &&
                m('div', [
                  m('h4', `${diff.different.length} results with differences:`),
                  m(
                    'ul',
                    diff.different
                      .slice(0, 10)
                      .map((item) =>
                        m('li', [
                          `ID: ${item.id}`,
                          m('br'),
                          `  SQL: TS=${item.result1Ts}, Track=${item.result1TrackUri}`,
                          m('br'),
                          `  Dataset: TS=${item.result2Ts}, Track=${item.result2TrackUri}`,
                        ]),
                      ),
                  ),
                  diff.different.length > 10 &&
                    m('p', `...and ${diff.different.length - 10} more`),
                ]),
            ]),
        ],
      ),
    ]);
  }

  private renderResults() {
    if (!this.results) return null;

    const {
      searchTerm,
      iterations,
      timestamp,
      sqlResults,
      datasetResults,
      sqlAvg,
      datasetAvg,
    } = this.results;

    return m('.pf-search-benchmark-results', [
      m('h2', 'Benchmark Results'),
      this.renderDifferenceCallout(),
      m('.pf-search-benchmark-summary', [
        m('p', `Search Term: "${searchTerm}"`),
        m('p', `Iterations: ${iterations}`),
        m('p', `Run at: ${timestamp.toLocaleString()}`),
      ]),
      m('.pf-search-benchmark-comparison', [
        m('h3', 'Performance Comparison'),
        m('table', [
          m('thead', [
            m('tr', [
              m('th', 'Method'),
              m('th', 'Average Duration (ms)'),
              m('th', 'Min (ms)'),
              m('th', 'Max (ms)'),
              m('th', 'Result Count'),
            ]),
          ]),
          m('tbody', [
            m('tr', [
              m('td', 'SQL Search'),
              m('td', sqlAvg.toFixed(2)),
              m(
                'td',
                Math.min(...sqlResults.map((r) => r.duration)).toFixed(2),
              ),
              m(
                'td',
                Math.max(...sqlResults.map((r) => r.duration)).toFixed(2),
              ),
              m('td', sqlResults[0]?.resultCount ?? 0),
            ]),
            m('tr', [
              m('td', 'Dataset Search'),
              m('td', datasetAvg.toFixed(2)),
              m(
                'td',
                Math.min(...datasetResults.map((r) => r.duration)).toFixed(2),
              ),
              m(
                'td',
                Math.max(...datasetResults.map((r) => r.duration)).toFixed(2),
              ),
              m('td', datasetResults[0]?.resultCount ?? 0),
            ]),
          ]),
        ]),
      ]),
      m('.pf-search-benchmark-details', [
        m('h3', 'Individual Run Results'),
        m('.pf-search-benchmark-methods', [
          this.renderMethodResults('SQL Search', sqlResults),
          this.renderMethodResults('Dataset Search', datasetResults),
        ]),
      ]),
    ]);
  }

  private renderMethodResults(methodName: string, results: BenchmarkResult[]) {
    return m('.pf-search-benchmark-method', [
      m('h4', methodName),
      m('table', [
        m('thead', [
          m('tr', [
            m('th', 'Run #'),
            m('th', 'Duration (ms)'),
            m('th', 'Results'),
          ]),
        ]),
        m(
          'tbody',
          results.map((result, index) =>
            m('tr', [
              m('td', index + 1),
              m('td', result.duration.toFixed(2)),
              m('td', result.resultCount),
            ]),
          ),
        ),
      ]),
    ]);
  }

  private async runBenchmark(trace: Trace) {
    this.results = undefined;
    this.running = true;
    this.showDiffDetails = false;

    const sqlResults: BenchmarkResult[] = [];
    const datasetResults: BenchmarkResult[] = [];
    let sqlResultsData: SearchResultEvent[] = [];
    let datasetResultsData: SearchResultEvent[] = [];

    try {
      // Run SQL search benchmark
      for (let i = 0; i < this.iterations; i++) {
        const startTime = performance.now();
        const result = await this.runSqlSearch(trace, this.searchTerm);
        const duration = performance.now() - startTime;
        sqlResults.push({
          method: 'SQL',
          duration,
          resultCount: result.length,
        });
        if (i === 0) {
          sqlResultsData = result;
        }
      }

      // Run dataset search benchmark
      for (let i = 0; i < this.iterations; i++) {
        const startTime = performance.now();
        const result = await this.runDatasetSearch(trace, this.searchTerm);
        const duration = performance.now() - startTime;
        datasetResults.push({
          method: 'Dataset',
          duration,
          resultCount: result.length,
        });
        if (i === 0) {
          datasetResultsData = result;
        }
      }

      // Calculate averages
      const sqlAvg =
        sqlResults.reduce((sum, r) => sum + r.duration, 0) / sqlResults.length;
      const datasetAvg =
        datasetResults.reduce((sum, r) => sum + r.duration, 0) /
        datasetResults.length;

      // Compare results using shared utility
      const differences = compareSearchResults(
        sqlResultsData,
        datasetResultsData,
      );

      this.results = {
        searchTerm: this.searchTerm,
        iterations: this.iterations,
        timestamp: new Date(),
        sqlResults,
        datasetResults,
        sqlAvg,
        datasetAvg,
        differences,
      };
    } finally {
      this.running = false;
    }
  }

  private async runSqlSearch(
    trace: Trace,
    searchTerm: string,
  ): Promise<SearchResultEvent[]> {
    const engine = trace.engine;
    const trackManager = trace.tracks as TrackManagerImpl;

    const searchResults = await executeSqlSearch(
      engine,
      trackManager,
      searchTerm,
    );

    const results: SearchResultEvent[] = [];
    for (let i = 0; i < searchResults.totalResults; i++) {
      results.push({
        id: searchResults.eventIds[i],
        ts: searchResults.tses[i],
        trackUri: searchResults.trackUris[i],
      });
    }

    return results;
  }

  private async runDatasetSearch(
    trace: Trace,
    searchTerm: string,
  ): Promise<SearchResultEvent[]> {
    const engine = trace.engine;
    const trackManager = trace.tracks;

    // Get the search providers from the trace's search manager
    // Access the internal _providers array from SearchManagerImpl
    const searchManager = trace.search as {
      _providers?: SearchProvider[];
    };
    const searchProviders = searchManager._providers ?? [];

    const allResults = await searchTrackEvents(
      engine,
      trackManager.getAllTracks(),
      searchProviders,
      searchTerm,
    );

    return allResults.map((r) => ({
      id: r.id,
      ts: r.ts,
      trackUri: r.track.uri,
    }));
  }
}
