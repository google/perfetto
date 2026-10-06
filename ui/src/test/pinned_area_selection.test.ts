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

import {expect, test, type Locator, type Page} from '@playwright/test';
import {PerfettoTestHelper} from './perfetto_ui_test_helper';
import type {TraceImpl} from '../core/trace_impl';

test.describe.configure({mode: 'serial'});

let pth: PerfettoTestHelper;
let page: Page;
let drawerPanel: Locator;
let pinButton: Locator;

interface PinState {
  readonly pinned: boolean;
  readonly kind: string;
  // Selection bounds in ns, or undefined if not an area selection.
  readonly start?: number;
  readonly end?: number;
  // Selection bounds as fractions of the visible window.
  readonly startFrac?: number;
  readonly endFrac?: number;
}

async function getState(): Promise<PinState> {
  return page.evaluate(() => {
    const trace = self.app.trace as TraceImpl;
    const sel = trace.selection.selection;
    const pinned = trace.pinnedAreaSelection.isPinned;
    if (sel.kind !== 'area') return {pinned, kind: sel.kind};
    const win = trace.timeline.visibleWindow;
    const winStart = win.start.toNumber();
    const start = Number(sel.start);
    const end = Number(sel.end);
    return {
      pinned,
      kind: sel.kind,
      start,
      end,
      startFrac: (start - winStart) / win.duration,
      endFrac: (end - winStart) / win.duration,
    };
  });
}

// Holds down a WASD key for a while, so the timeline pans/zooms noticeably.
// Sends repeated keydowns, like the OS auto-repeat does while a key is held.
async function holdKey(key: string, ms = 300) {
  const start = Date.now();
  while (Date.now() - start < ms) {
    await page.keyboard.down(key);
    await page.waitForTimeout(50);
  }
  await page.keyboard.up(key);
  await pth.waitForPerfettoIdle();
}

async function visibleWindowStart(): Promise<number> {
  return page.evaluate(() =>
    self.app.trace!.timeline.visibleWindow.start.toNumber(),
  );
}

async function aggregationText(): Promise<string> {
  await pth.waitForPerfettoIdle();
  return drawerPanel.innerText();
}

test.beforeAll(async ({browser}) => {
  page = await browser.newPage();
  pth = new PerfettoTestHelper(page);
  await pth.openTraceFile('api34_startup_cold.perfetto-trace');
  drawerPanel = page.locator('.pf-drawer-panel__drawer');
  pinButton = drawerPanel.locator('button[label="Pin to viewport"]');
});

test('pin button is shown for area selections', async () => {
  // Select an area across the CPU tracks.
  await page.mouse.move(600, 250);
  await page.mouse.down();
  await page.mouse.move(800, 350);
  await page.mouse.up();
  await pth.waitForPerfettoIdle();

  await expect(pinButton).toBeVisible();
  await expect(pinButton).not.toHaveClass(/pf-active/);
  expect((await getState()).pinned).toBe(false);
});

test('A and D step the pinned selection by its own duration', async () => {
  // Select an area away from the trace edges (so there is room to step) where
  // there is CPU activity (so the aggregation changes).
  await page.evaluate(() => {
    const trace = self.app.trace!;
    const {start, end} = trace.traceInfo;
    const center = Number(start) + Number(end - start) * 0.8;
    const win = trace.timeline.visibleWindow;
    trace.timeline.pan(center - (win.start.toNumber() + win.duration / 2));
    trace.timeline.zoom(1e9 / trace.timeline.visibleWindow.duration);
  });
  await page.mouse.move(600, 250);
  await page.mouse.down();
  await page.mouse.move(800, 350);
  await page.mouse.up();
  await pth.waitForPerfettoIdle();

  const first = await getState();
  const aggregationBefore = await aggregationText();
  expect(first.kind).toBe('area');
  const dur = first.end! - first.start!;

  await pinButton.click();
  await expect(pinButton).toHaveClass(/pf-active/);
  expect((await getState()).pinned).toBe(true);

  // Each tap selects the adjacent [start, end) interval, and the timeline
  // follows so the selection stays in place on screen.
  let prev = first;
  for (let i = 0; i < 3; i++) {
    await page.keyboard.press('d');
    const cur = await getState();
    expect(cur.start).toBe(prev.end);
    expect(cur.end! - cur.start!).toBe(dur);
    expect(cur.startFrac!).toBeCloseTo(first.startFrac!, 4);
    prev = cur;
  }
  for (let i = 0; i < 3; i++) {
    await page.keyboard.press('a');
    const cur = await getState();
    expect(cur.end).toBe(prev.start);
    prev = cur;
  }
  expect(prev.start).toBe(first.start);

  // Holding a key keeps stepping, staying on the same grid.
  await holdKey('d');
  const held = await getState();
  expect(held.end! - held.start!).toBe(dur);
  expect((held.start! - first.start!) % dur).toBe(0);
  expect(held.start!).toBeGreaterThan(first.start!);

  // The details panel isn't recreated while pinned, so the pin button keeps
  // focus. Destroying the focused element can stop key auto-repeat, which
  // halts WASD navigation mid-press.
  await expect(pinButton).toBeFocused();

  // The aggregation reflects the new selection.
  expect(await aggregationText()).not.toEqual(aggregationBefore);
});

test('zooming with WASD resizes the pinned selection', async () => {
  const before = await getState();
  const winBefore = await visibleWindowStart();
  const center = (s: PinState) => (s.start! + s.end!) / 2;

  await page.mouse.move(700, 300);
  await holdKey('w');
  const shrunk = await getState();
  expect(await visibleWindowStart()).toBe(winBefore);
  expect(shrunk.pinned).toBe(true);
  expect(shrunk.end! - shrunk.start!).toBeLessThan(before.end! - before.start!);
  expect(center(shrunk)).toBeCloseTo(center(before), -2);

  await holdKey('s');
  const grown = await getState();
  expect(await visibleWindowStart()).toBe(winBefore);
  expect(grown.end! - grown.start!).toBeGreaterThan(
    shrunk.end! - shrunk.start!,
  );
});

test('collapsing the details panel unpins', async () => {
  // Still pinned from the previous test.
  expect((await getState()).pinned).toBe(true);

  // Move away from the pin button so its tooltip doesn't cover the toggle.
  await page.mouse.move(0, 0);
  await page.locator('button[title="Hide panel"]').click();
  await pth.waitForPerfettoIdle();
  expect((await getState()).pinned).toBe(false);

  const before = await getState();
  await holdKey('d');
  const after = await getState();
  expect(after.kind).toBe('area');
  expect(after.start).toBe(before.start);
  expect(after.end).toBe(before.end);

  // Reopening the panel shows the selection, but unpinned.
  await page.locator('button[title="Show panel"]').click();
  await pth.waitForPerfettoIdle();
  await expect(pinButton).toBeVisible();
  await expect(pinButton).not.toHaveClass(/pf-active/);
});

test('at the end of the trace the pinned selection keeps moving', async () => {
  // Move the timeline to the end of the trace and select an area there.
  await page.evaluate(() => self.app.trace!.timeline.pan(Number.MAX_VALUE));
  await pth.waitForPerfettoIdle();
  await page.mouse.move(900, 250);
  await page.mouse.down();
  await page.mouse.move(1100, 350);
  await page.mouse.up();
  await pth.waitForPerfettoIdle();
  await pinButton.click();

  const before = await getState();
  const winBefore = await visibleWindowStart();
  const traceEnd = await page.evaluate(() =>
    Number(self.app.trace!.traceInfo.end),
  );

  // The timeline can't move, so the selection steps to the end of the trace,
  // where the last step is clipped to the trace.
  await holdKey('d', 2000);
  const after = await getState();
  const dur = before.end! - before.start!;
  expect(await visibleWindowStart()).toBe(winBefore);
  expect(after.pinned).toBe(true);
  expect(after.end).toBe(traceEnd);
  expect((after.start! - before.start!) % dur).toBe(0);
  expect(after.startFrac!).toBeGreaterThan(before.startFrac!);

  // Stepping back moves the timeline under the selection again.
  await holdKey('a');
  const back = await getState();
  expect(await visibleWindowStart()).toBeLessThan(winBefore);
  expect(back.startFrac!).toBeCloseTo(after.startFrac!, 4);
});
