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
let editButton: Locator;
let timelinePx: {left: number; width: number};

function fracToPx(frac: number): number {
  return timelinePx.left + frac * timelinePx.width;
}

interface SelectionState {
  readonly editing: boolean;
  readonly kind: string;
  readonly start?: number;
  readonly end?: number;
  readonly startFrac?: number;
  readonly endFrac?: number;
}

async function getState(): Promise<SelectionState> {
  return page.evaluate(() => {
    const trace = self.app.trace as TraceImpl;
    const sel = trace.selection.selection;
    const editing = trace.areaSelectionEditor.isEditing;
    if (sel.kind !== 'area') return {editing, kind: sel.kind};
    const win = trace.timeline.visibleWindow;
    const winStart = win.start.toNumber();
    const start = Number(sel.start);
    const end = Number(sel.end);
    return {
      editing,
      kind: sel.kind,
      start,
      end,
      startFrac: (start - winStart) / win.duration,
      endFrac: (end - winStart) / win.duration,
    };
  });
}

// Sends repeated keydowns like OS auto-repeat while a key is held.
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

async function selectArea(x1 = 600, x2 = 800) {
  await page.mouse.move(x1, 250);
  await page.mouse.down();
  await page.mouse.move(x2, 350);
  await page.mouse.up();
  await pth.waitForPerfettoIdle();
}

async function cursorAt(x: number, y: number): Promise<string> {
  await page.mouse.move(x, y);
  await pth.waitForPerfettoIdle();
  return page.evaluate(
    (p) => getComputedStyle(document.elementFromPoint(p.x, p.y)!).cursor,
    {x, y},
  );
}

async function dragSelection(fromX: number, toX: number, y = 300) {
  await page.mouse.move(fromX, y);
  await page.mouse.down();
  await page.mouse.move(toX, y + 5, {steps: 5});
  await page.mouse.up();
  await pth.waitForPerfettoIdle();
}

async function startEditing() {
  await page.mouse.move(650, 300);
  await editButton.click();
  await expect(editButton.locator('button')).toHaveClass(/pf-active/);
  expect((await getState()).editing).toBe(true);
}

test.beforeAll(async ({browser}) => {
  page = await browser.newPage();
  pth = new PerfettoTestHelper(page);
  await pth.openTraceFile('api34_startup_cold.perfetto-trace');
  drawerPanel = page.locator('.pf-drawer-panel__drawer');
  editButton = page.locator('.pf-area-selection-edit-button');

  // Show 1s away from the trace edges (so there is room to move the
  // selection) where there is CPU activity (so the aggregation changes).
  await page.evaluate(() => {
    const trace = self.app.trace!;
    const {start, end} = trace.traceInfo;
    const center = Number(start) + Number(end - start) * 0.8;
    const win = trace.timeline.visibleWindow;
    trace.timeline.pan(center - (win.start.toNumber() + win.duration / 2));
    trace.timeline.zoom(1e9 / trace.timeline.visibleWindow.duration);
  });
  await pth.waitForPerfettoIdle();
});

test('hovering over the area selection shows the edit button', async () => {
  await selectArea();
  const first = await getState();
  timelinePx = {
    left: 600 - (first.startFrac! * 200) / (first.endFrac! - first.startFrac!),
    width: 200 / (first.endFrac! - first.startFrac!),
  };

  // Not shown while on the edge of the selection, to not get in the way of
  // resizing it.
  await expect(editButton).toBeHidden();

  await page.mouse.move(650, 300);
  await expect(editButton).toBeVisible();
  await expect(editButton).toHaveCSS('opacity', '0.2');
  const box = (await editButton.boundingBox())!;
  expect(box.x + box.width / 2).toBeCloseTo(700, -1);
  expect(box.y + box.height / 2).toBeCloseTo(300, -1);
  expect(box.width).toBeGreaterThanOrEqual(28);

  await editButton.hover();
  await expect(editButton).toHaveCSS('opacity', '1');

  await page.mouse.move(500, 300);
  await expect(editButton).toBeHidden();
  expect((await getState()).editing).toBe(false);
});

test('the cursor shows what dragging does', async () => {
  expect(await cursorAt(650, 300)).toBe('move');
  expect(await cursorAt(600, 300)).toBe('col-resize');
  // Outside the selected tracks, dragging makes a new selection.
  expect(await cursorAt(650, 415)).not.toBe('move');
  await expect(editButton).toBeHidden();

  await page.mouse.move(650, 300);
  const box = (await editButton.boundingBox())!;
  expect(await cursorAt(box.x + box.width / 2, box.y + box.height / 2)).toBe(
    'pointer',
  );
});

test('dragging inside the selection moves it', async () => {
  const before = await getState();
  const winBefore = await visibleWindowStart();
  const dur = before.end! - before.start!;

  await dragSelection(620, 720);

  const moved = await getState();
  expect(moved.kind).toBe('area');
  expect(moved.editing).toBe(false);
  expect(moved.end! - moved.start!).toBe(dur);
  expect(fracToPx(moved.startFrac!) - fracToPx(before.startFrac!)).toBeCloseTo(
    100,
    0,
  );
  expect(await visibleWindowStart()).toBe(winBefore);

  await dragSelection(720, 620);
  const back = await getState();
  expect(back.end! - back.start!).toBe(dur);
  expect(
    Math.abs(fracToPx(back.startFrac!) - fracToPx(before.startFrac!)),
  ).toBeLessThan(1);

  await dragSelection(620, 720, 415);
  const other = await getState();
  expect(other.kind).toBe('area');
  expect(other.start).not.toBe(before.start);
  await selectArea();

  await page.mouse.click(620, 300);
  await pth.waitForPerfettoIdle();
  expect((await getState()).kind).not.toBe('area');
  await selectArea();
});

test('the edit button toggles edit mode', async () => {
  await startEditing();
  await editButton.click();
  await expect(editButton.locator('button')).not.toHaveClass(/pf-active/);
  expect((await getState()).editing).toBe(false);
});

test('A and D step the selection by its own duration', async () => {
  await startEditing();
  const first = await getState();
  const aggregationBefore = await aggregationText();
  const dur = first.end! - first.start!;

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

  await holdKey('d');
  const held = await getState();
  expect(held.editing).toBe(true);
  expect(held.end! - held.start!).toBe(dur);
  expect((held.start! - first.start!) % dur).toBe(0);
  expect(held.start!).toBeGreaterThan(first.start!);

  expect(await aggregationText()).not.toEqual(aggregationBefore);
});

test('W and S resize the selection', async () => {
  const before = await getState();
  const winBefore = await visibleWindowStart();
  const center = (s: SelectionState) => (s.start! + s.end!) / 2;

  await holdKey('w');
  const shrunk = await getState();
  expect(await visibleWindowStart()).toBe(winBefore);
  expect(shrunk.editing).toBe(true);
  expect(shrunk.end! - shrunk.start!).toBeLessThan(before.end! - before.start!);
  expect(center(shrunk)).toBeCloseTo(center(before), -2);

  await holdKey('s');
  const grown = await getState();
  expect(await visibleWindowStart()).toBe(winBefore);
  expect(grown.end! - grown.start!).toBeGreaterThan(
    shrunk.end! - shrunk.start!,
  );
});

test('dragging inside the selection re-anchors it', async () => {
  const before = await getState();
  const dur = before.end! - before.start!;
  expect(before.editing).toBe(true);

  const x = fracToPx(before.startFrac!) + 10;
  await dragSelection(x, x - 50);

  const moved = await getState();
  expect(moved.editing).toBe(true);
  expect(moved.startFrac!).toBeLessThan(before.startFrac!);

  await page.keyboard.press('d');
  const stepped = await getState();
  expect(stepped.start).toBe(moved.end);
  expect(stepped.end! - stepped.start!).toBe(dur);
  expect(stepped.startFrac!).toBeCloseTo(moved.startFrac!, 4);
});

test('Escape exits edit mode', async () => {
  expect((await getState()).editing).toBe(true);
  await page.keyboard.press('Escape');
  await pth.waitForPerfettoIdle();
  const state = await getState();
  expect(state.editing).toBe(false);
  expect(state.kind).toBe('empty');
  await expect(editButton).toBeHidden();
});

test('collapsing the details panel exits edit mode', async () => {
  await selectArea();
  await startEditing();

  await page.mouse.move(0, 0);
  await page.locator('button[title="Hide panel"]').click();
  await pth.waitForPerfettoIdle();
  expect((await getState()).editing).toBe(false);

  const before = await getState();
  await holdKey('d');
  const after = await getState();
  expect(after.kind).toBe('area');
  expect(after.start).toBe(before.start);
  expect(after.end).toBe(before.end);

  await page.locator('button[title="Show panel"]').click();
  await pth.waitForPerfettoIdle();
  expect((await getState()).editing).toBe(false);
});

test('at the end of the trace the selection keeps moving', async () => {
  await page.evaluate(() => self.app.trace!.timeline.pan(Number.MAX_VALUE));
  await pth.waitForPerfettoIdle();
  await selectArea(900, 1100);
  await page.mouse.move(950, 300);
  await editButton.click();

  const before = await getState();
  expect(before.editing).toBe(true);
  const winBefore = await visibleWindowStart();
  const traceEnd = await page.evaluate(() =>
    Number(self.app.trace!.traceInfo.end),
  );

  await holdKey('d', 2000);
  const after = await getState();
  const dur = before.end! - before.start!;
  expect(await visibleWindowStart()).toBe(winBefore);
  expect(after.editing).toBe(true);
  expect(after.end).toBe(traceEnd);
  expect((after.start! - before.start!) % dur).toBe(0);
  expect(after.startFrac!).toBeGreaterThan(before.startFrac!);

  await holdKey('a');
  const back = await getState();
  expect(await visibleWindowStart()).toBeLessThan(winBefore);
  expect(back.startFrac!).toBeCloseTo(after.startFrac!, 4);
});
