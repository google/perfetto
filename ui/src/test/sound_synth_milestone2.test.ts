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

// Smoke test for the SoundSynth plugin (Trace-To-Techno) milestone 2:
// exercises the new block parameter panels and colored ports by:
//   1. Loading a trace
//   2. Navigating to the sound_synth page
//   3. Adding a preset instrument from the picker
//   4. Opening the instrument editor
//   5. Adding several blocks via the palette so each new panel renders
//   6. Clicking a drawbar organ preset chip
//
// The whole flow runs as a single Playwright test so we don't depend on
// page state persisting between sibling tests in serial mode.

import {test, expect} from '@playwright/test';
import {PerfettoTestHelper} from './perfetto_ui_test_helper';

test('sound synth milestone 2: panels + ports', async ({browser}) => {
  test.setTimeout(120_000);
  const page = await browser.newPage();
  const pth = new PerfettoTestHelper(page);

  // Any small trace works: the plugin only needs `onTraceLoad` to fire.
  await pth.openTraceFile('api24_startup_cold.perfetto-trace');
  await pth.navigate('#!/sound_synth');

  // Wait for the preset library fetch + parse to settle.
  await page.waitForSelector('.sound-synth-page');
  await page.waitForFunction(() =>
    !document.querySelector('.sound-synth-page')!.textContent!.includes(
      'Loading track data and preset library',
    ),
  );
  await pth.waitForPerfettoIdle();

  await expect(page.locator('.rack-canvas-wrapper')).toBeVisible();
  await expect(page.locator('.instrument-canvas-wrapper')).toBeVisible();
  await pth.waitForIdleAndScreenshot('00_initial.png');

  // --- Add an instrument from the preset picker. ---
  await page.getByRole('button', {name: '+ Instrument'}).click();
  await expect(page.locator('.preset-picker')).toBeVisible();
  // Filter to "lead" so we get a representative chain.
  await page.locator('.preset-picker-cats button', {hasText: 'Lead'}).click();
  await page.locator('.preset-entry').first().click();
  await expect(page.locator('.preset-picker')).toHaveCount(0);
  await pth.waitForPerfettoIdle();
  await pth.waitForIdleAndScreenshot('01_preset_loaded.png');

  // --- Open the instrument editor by clicking the Edit button. ---
  await page.getByRole('button', {name: 'Edit'}).first().click();
  await expect(page.locator('.instrument-toolbar')).toBeVisible();
  await pth.waitForPerfettoIdle();
  await pth.waitForIdleAndScreenshot('02_editor_open.png');

  // --- Add new blocks via the palette so every milestone-2 panel renders. ---
  const newBlocks = [
    'FM Osc',
    'SuperSaw',
    'Wavetable',
    'Noise',
    'LFO',
    'Delay',
    'Chorus',
    'Drawbar Organ',
    'Phase Dist',
    'Fold Osc',
    'Sync Osc',
  ];

  for (const name of newBlocks) {
    if (!(await page.locator('.block-palette').isVisible())) {
      await page.getByRole('button', {name: '+ Add Block'}).click();
      await expect(page.locator('.block-palette')).toBeVisible();
    }
    await page.locator('.block-palette button', {hasText: name})
      .first().click();
    await pth.waitForPerfettoIdle();
  }

  // Close the palette if still open so it doesn't cover the canvas.
  if (await page.locator('.block-palette').isVisible()) {
    await page.getByRole('button', {name: 'Close Palette'}).click();
  }
  await pth.waitForIdleAndScreenshot('03_all_blocks.png');

  // --- Drawbar organ: verify panel rendered + click the "Jazz" chip. ---
  // The drawbar organ may be placed off-screen on the canvas, so we
  // scroll its node into view and use `force: true` to bypass the
  // canvas hit-testing that can otherwise hide hits behind nodes.
  const drawbarNode = page.locator('.pf-node', {hasText: 'Drawbar Organ'});
  await expect(drawbarNode).toHaveCount(1);
  await drawbarNode.scrollIntoViewIfNeeded();
  // The Jazz preset chip should exist as a button inside the drawbar node.
  const jazzBtn = drawbarNode.locator('button', {hasText: 'Jazz'});
  await expect(jazzBtn).toHaveCount(1);
  await jazzBtn.click({force: true});
  await pth.waitForPerfettoIdle();
  await pth.waitForIdleAndScreenshot('04_drawbar_jazz.png');

  // --- High-res focused screenshots of individual panels. ---
  // These are not a snapshot comparison — they're written to the test
  // results directory for visual inspection. We restrict to nodes
  // inside the instrument editor to avoid accidentally matching the
  // rack-level instrument card (whose chain preview text can contain
  // block names like "Delay").
  const instrumentCanvas = page.locator('.instrument-canvas-container');
  for (const name of ['Drawbar Organ', 'Chorus', 'Delay', 'LFO',
       'Wavetable', 'FM Osc', 'SuperSaw', 'Noise']) {
    // Match by the .pf-node-title element exactly (not arbitrary text
    // inside the body) so e.g. "Delay" doesn't pick up the Chorus
    // node whose description mentions "delay".
    const node = instrumentCanvas
      .locator('.pf-node')
      .filter({has: page.locator('.pf-node-title', {hasText: name})})
      .first();
    if (await node.count() === 0) continue;
    await node.scrollIntoViewIfNeeded();
    const safe = name.replace(/\s+/g, '_').toLowerCase();
    await node.screenshot({
      path: `../out/ui/ui-test-results/panel_${safe}.png`,
      scale: 'css',
    });
  }
  // Whole-page full screenshot (no clip) for the final overview.
  await page.screenshot({
    path: '../out/ui/ui-test-results/sound_synth_full_page.png',
    fullPage: true,
  });
});
