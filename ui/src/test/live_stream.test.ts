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

// com.google.LiveStream against the demo server's backend, which answers the
// plugin's api/ requests in process. Assertions use what outlasts redesigns
// of the plugin's controls: the trace, its workspace, the plugin's tables and
// the area selection tabs. Only LiveControls knows the controls' DOM.

import {expect, test, type Page, type Route} from '@playwright/test';
import type {time} from '../base/time';
import {
  DemoBackend,
  uiPath,
} from '../plugins/com.google.LiveStream/demo/demo_backend';

const TARGET = 'job-1/trainer';
const ERROR = 'Cannot reach the target';
const SEC = 1e9;

// The plugin's controls: update these selectors when they change.
class LiveControls {
  constructor(private readonly page: Page) {}

  // The status shown: "live", "buffering", "paused" or "error".
  status(): Promise<string> {
    return this.page.evaluate(() => {
      const badge = document.querySelector(
        '.pf-live-stream-hud [class*="__badge--"]',
      );
      return /__badge--(\w+)/.exec(badge?.className ?? '')?.[1] ?? '';
    });
  }

  toggleFollow(): Promise<void> {
    return this.button(/^Follow Live/).click();
  }

  togglePause(): Promise<void> {
    return this.button(/^(Pause|Resume Live)$/).click();
  }

  private button(text: RegExp) {
    return this.page.locator('.pf-live-stream-hud button', {hasText: text});
  }
}

test.describe.configure({mode: 'serial'});

let backend: DemoBackend;
let page: Page;
let controls: LiveControls;
let streamRequests = 0;

test.beforeAll(async ({browser}) => {
  backend = new DemoBackend({seed: 1, tz: 'UTC', backfill: 30});
  page = await browser.newPage();
  controls = new LiveControls(page);
  await page.route('**/api/**', serve);
});

test.afterAll(async () => {
  backend.dispose();
  await page.close();
});

// Answers a request of the page with the backend.
async function serve(route: Route) {
  const url = route.request().url();
  const reply = backend.handle(url);
  if (reply === undefined) {
    await route.fallback();
    return;
  }
  if (new URL(url).pathname === '/api/stream') streamRequests++;
  const {status, headers, body} = await reply;
  await route
    .fulfill({status, headers, body: Buffer.from(body)})
    // The page may have dropped the request meanwhile, e.g. on pause.
    .catch(() => undefined);
}

// The names of the server's tracks, in display order.
async function serverTracks(): Promise<string[]> {
  const reply = await backend.handle(`/api/stream?target=${TARGET}&window=1`);
  const text = new TextDecoder().decode(reply?.body);
  const body = JSON.parse(text) as {tracks: Array<{name: string}>};
  return body.tracks.map((t) => t.name);
}

function traceState() {
  return page.evaluate(() => {
    const trace = window.app.trace;
    const tracks = trace?.currentWorkspace.flatTracksOrdered ?? [];
    return {
      tracks: tracks.map((t) => t.name),
      end: Number(trace?.traceInfo.end ?? 0),
      visibleEnd: Number(trace?.timeline.visibleWindow.end.integral ?? 0),
    };
  });
}

async function traceEnd(): Promise<number> {
  return (await traceState()).end;
}

async function visibleEnd(): Promise<number> {
  return (await traceState()).visibleEnd;
}

// Waits until the trace grew by over a second.
async function waitForGrowth() {
  const end = await traceEnd();
  await expect.poll(traceEnd).toBeGreaterThan(end + SEC);
}

// The first column of the first row of `query`, as a number.
function sqlNumber(query: string): Promise<number> {
  return page.evaluate(async (query) => {
    const result = await window.app.trace?.engine.query(query);
    const columns = result?.columns() ?? [];
    return Number(result?.iter({}).get(columns[0]!) ?? NaN);
  }, query);
}

test('opens the target with the server tracks, live', async () => {
  await page.goto(uiPath(TARGET));
  await expect.poll(() => controls.status(), {timeout: 30_000}).toBe('live');
  const tracks = await serverTracks();
  expect(tracks).toContain('Worker 3');
  await expect.poll(async () => (await traceState()).tracks).toEqual(tracks);

  const urlTarget = await page.evaluate(() =>
    new URLSearchParams(location.search).get('com.google.LiveStream:target'),
  );
  expect(urlTarget).toBe(TARGET);
  const steps = `SELECT COUNT(*) FROM __live_stream_slice WHERE name = 'train_step'`;
  expect(await sqlNumber(steps)).toBeGreaterThan(10);
  const points = 'SELECT COUNT(*) FROM __live_stream_counter';
  expect(await sqlNumber(points)).toBeGreaterThan(10);
  const lanes = 'SELECT COUNT(*) FROM __live_stream_track';
  expect(await sqlNumber(lanes)).toBe(tracks.length);
});

test('follows the growing end until told not to', async () => {
  const start = await visibleEnd();
  await waitForGrowth();
  expect(await visibleEnd()).toBeGreaterThan(start);

  await controls.toggleFollow();
  await page.evaluate(() => new Promise(requestAnimationFrame));
  const stopped = await visibleEnd();
  await waitForGrowth();
  expect(await visibleEnd()).toBe(stopped);

  await controls.toggleFollow();
  await expect.poll(visibleEnd).toBeGreaterThan(stopped + SEC);
});

test('stops requesting while paused', async () => {
  await controls.togglePause();
  await expect.poll(() => controls.status()).toBe('paused');
  // Lets a request in flight finish.
  await page.waitForTimeout(1000);
  const requests = streamRequests;
  const end = await traceEnd();
  await page.waitForTimeout(2000);
  expect([streamRequests, await traceEnd()]).toEqual([requests, end]);

  await controls.togglePause();
  await expect.poll(() => controls.status()).toBe('live');
  await waitForGrowth();
});

test('aggregates area selections', async () => {
  await page.evaluate(() => {
    const trace = window.app.trace;
    if (trace === undefined) return;
    const end = trace.traceInfo.end;
    trace.selection.selectArea({
      start: (end - 10_000_000_000n) as time,
      end,
      trackUris: trace.currentWorkspace.flatTracksOrdered.flatMap((t) =>
        t.uri === undefined ? [] : [t.uri],
      ),
    });
  });
  const area = page.locator('.pf-details-shell', {
    has: page.getByRole('heading', {name: 'Area Selection'}),
  });
  const tab = (name: string) => area.getByRole('button', {name, exact: true});
  const cell = (name: string) => area.getByRole('cell', {name, exact: true});
  await tab('Slices').click();
  await expect(cell('train_step')).toBeVisible();
  await tab('Counters').click();
  await expect(cell('Loss')).toBeVisible();
});

test('shows buffering and errors', async () => {
  // Buffering shows after 5 s without data, and a status delay.
  backend.set([['stall', '1']]);
  await expect
    .poll(() => controls.status(), {timeout: 15_000})
    .toBe('buffering');
  backend.set([['stall', '0']]);
  await expect.poll(() => controls.status()).toBe('live');

  backend.set([['error', ERROR]]);
  await expect.poll(() => controls.status()).toBe('error');
  await expect(page.getByText(ERROR).first()).toBeVisible();
  backend.set([['error', '']]);
  await expect.poll(() => controls.status()).toBe('live');
});

test('shows errors before the first data, then goes live', async () => {
  // A new stream that gets no data: the page opens with the error.
  backend.set([['error', ERROR]]);
  await backend.handle(`/demo/restart?target=${TARGET}`);
  await page.reload();
  await expect.poll(() => controls.status(), {timeout: 15_000}).toBe('error');
  await expect(page.getByText(ERROR).first()).toBeVisible();

  // Retried with backoff.
  backend.set([['error', '']]);
  await expect.poll(() => controls.status(), {timeout: 30_000}).toBe('live');
});
