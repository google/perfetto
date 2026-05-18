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

// Engine startup benchmark — drives engine_bench.html, harvests the timings
// it exposes on `window.__benchResults`, and prints a JSON summary.
//
// Disabled by default. Enable with `PERFETTO_BENCH=1`, and run only after
// building the UI with `ui/build --enable-engine-bench` so the bench bundle
// and page are present in dist.
//
// Tunables:
//   PERFETTO_BENCH_N        iterations (default 20)
//   PERFETTO_BENCH_WARMUP   warmup iterations excluded from stats (default 2)
//   PERFETTO_BENCH_COLD     "1" to cache-bust each iteration

import {test, expect} from '@playwright/test';

interface PhaseStats {
  readonly min: number;
  readonly p50: number;
  readonly p95: number;
  readonly max: number;
  readonly mean: number;
}

interface BenchResults {
  readonly config: {n: number; warmup: number; cold: boolean};
  readonly samples: ReadonlyArray<Record<string, number>>;
  readonly summary: Record<string, PhaseStats>;
}

const enabled = process.env.PERFETTO_BENCH === '1';
const N = Number(process.env.PERFETTO_BENCH_N ?? '20');
const WARMUP = Number(process.env.PERFETTO_BENCH_WARMUP ?? '2');
const COLD = process.env.PERFETTO_BENCH_COLD === '1';

const describeFn = enabled ? test.describe : test.describe.skip;

describeFn('engine startup benchmark', () => {
  test.setTimeout(5 * 60_000);

  test('measure cold/warm startup latencies', async ({browser}) => {
    const page = await browser.newPage();
    page.on('console', (msg) => {
      if (msg.type() === 'warning' || msg.type() === 'error') {
        // eslint-disable-next-line no-console
        console.log(`[page ${msg.type()}] ${msg.text()}`);
      }
    });

    const url =
      `/engine_bench.html?auto=1&n=${N}&warmup=${WARMUP}` +
      (COLD ? '&cache=cold' : '');
    await page.goto(url);

    await page.waitForFunction(
      () =>
        (window as unknown as {__benchResults?: BenchResults}).__benchResults
          !== undefined,
      undefined,
      {timeout: 5 * 60_000},
    );

    const results = (await page.evaluate(
      () => (window as unknown as {__benchResults: BenchResults}).__benchResults,
    )) as BenchResults;

    expect(results.samples.length).toBe(N);

    // eslint-disable-next-line no-console
    console.log('\n=== engine_startup_bench results ===');
    // eslint-disable-next-line no-console
    console.log(JSON.stringify(results, null, 2));
    // eslint-disable-next-line no-console
    console.log('=== end engine_startup_bench results ===\n');
  });
});
