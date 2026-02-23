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
import {AsyncLimiter} from '../../base/async_limiter';
import {
  AreaSelection,
  AreaSelectionTab,
  areaSelectionsEqual,
} from '../../public/selection';
import {Trace} from '../../public/trace';
import {Button} from '../../widgets/button';
import {EmptyState} from '../../widgets/empty_state';
import {Spinner} from '../../widgets/spinner';
import {BenchmarkResult, runBenchmarks} from './benchmark';

function formatApproach(approach: string): string {
  switch (approach) {
    case 'uri_string':
      return 'URI String';
    case 'track_index':
      return 'Track Index';
    case 'groupid':
      return 'GroupID';
    case 'no_lineage':
      return 'No Lineage';
    default:
      return approach;
  }
}

function formatMs(ms: number): string {
  return ms.toFixed(2);
}

interface BenchmarkTableAttrs {
  results: BenchmarkResult[];
}

class BenchmarkTable implements m.ClassComponent<BenchmarkTableAttrs> {
  view({attrs}: m.CVnode<BenchmarkTableAttrs>) {
    const {results} = attrs;

    return m(
      'table.pf-benchmark-table',
      {
        style: {
          width: '100%',
          borderCollapse: 'collapse',
          fontFamily: 'monospace',
          fontSize: '12px',
        },
      },
      [
        m(
          'thead',
          m('tr', [
            m('th', {style: {textAlign: 'left', padding: '8px'}}, 'Approach'),
            m('th', {style: {textAlign: 'right', padding: '8px'}}, 'Tracks'),
            m('th', {style: {textAlign: 'right', padding: '8px'}}, 'Rows'),
            m(
              'th',
              {style: {textAlign: 'right', padding: '8px'}},
              'Build (ms)',
            ),
            m(
              'th',
              {style: {textAlign: 'right', padding: '8px'}},
              'Execute (ms)',
            ),
            m(
              'th',
              {style: {textAlign: 'right', padding: '8px'}},
              'Total (ms)',
            ),
          ]),
        ),
        m(
          'tbody',
          results.map((r) =>
            m('tr', {style: {borderTop: '1px solid var(--sys-border)'}}, [
              m('td', {style: {padding: '8px'}}, formatApproach(r.approach)),
              m(
                'td',
                {style: {textAlign: 'right', padding: '8px'}},
                r.trackCount,
              ),
              m(
                'td',
                {style: {textAlign: 'right', padding: '8px'}},
                r.rowCount.toLocaleString(),
              ),
              m(
                'td',
                {style: {textAlign: 'right', padding: '8px'}},
                formatMs(r.queryBuildTimeMs),
              ),
              m(
                'td',
                {style: {textAlign: 'right', padding: '8px'}},
                formatMs(r.queryExecuteTimeMs),
              ),
              m(
                'td',
                {
                  style: {
                    textAlign: 'right',
                    padding: '8px',
                    fontWeight: 'bold',
                  },
                },
                formatMs(r.totalTimeMs),
              ),
            ]),
          ),
        ),
      ],
    );
  }
}

export function createBenchmarkTab(trace: Trace): AreaSelectionTab {
  const limiter = new AsyncLimiter();
  let currentSelection: AreaSelection | undefined;
  let results: BenchmarkResult[] | undefined;
  let isLoading = false;

  return {
    id: 'aggregation_benchmark',
    name: 'Benchmark',
    render(selection: AreaSelection) {
      const selectionChanged =
        currentSelection === undefined ||
        !areaSelectionsEqual(selection, currentSelection);

      if (selectionChanged) {
        currentSelection = selection;
        results = undefined;
      }

      const runBenchmark = () => {
        limiter.schedule(async () => {
          isLoading = true;
          results = undefined;
          results = await runBenchmarks(trace.engine, selection.tracks);
          isLoading = false;
        });
      };

      if (isLoading) {
        return {
          isLoading: true,
          content: m(
            EmptyState,
            {
              icon: 'speed',
              title: 'Running benchmark...',
            },
            m(Spinner, {easing: true}),
          ),
        };
      }

      if (!results) {
        return {
          isLoading: false,
          content: m('div', {style: {padding: '16px', textAlign: 'center'}}, [
            m('p', `Selected ${selection.tracks.length} tracks`),
            m(Button, {
              label: 'Run Benchmark',
              onclick: runBenchmark,
            }),
          ]),
        };
      }

      return {
        isLoading: false,
        content: m('div', {style: {padding: '16px'}}, [
          m('h3', {style: {margin: '0 0 16px 0'}}, 'Benchmark Results'),
          m(BenchmarkTable, {results}),
          m(
            'div',
            {style: {marginTop: '16px'}},
            m(Button, {
              label: 'Re-run Benchmark',
              onclick: runBenchmark,
            }),
          ),
        ]),
      };
    },
  };
}
