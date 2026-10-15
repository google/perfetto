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

import {expect, test, type Page} from '@playwright/test';
import {ensureExists} from '../base/assert';
import {PerfettoTestHelper} from './perfetto_ui_test_helper';

async function sampleTrack(
  page: Page,
  clip: {x: number; y: number; width: number; height: number},
): Promise<number[][]> {
  const png = await page.screenshot({clip, scale: 'css'});
  return page.evaluate(
    async (src) => {
      const image = new Image();
      image.src = src;
      await image.decode();
      const canvas = document.createElement('canvas');
      canvas.width = image.width;
      canvas.height = image.height;
      const ctx = canvas.getContext('2d');
      if (ctx === null) throw new Error('Could not sample the screenshot');
      ctx.drawImage(image, 0, 0);
      const x = Math.floor(image.width / 2);
      // Sample the padding and slice fill above the text
      return [1, 5].map((y) =>
        Array.from(ctx.getImageData(x, y, 1, 1).data).slice(0, 3),
      );
    },
    `data:image/png;base64,${png.toString('base64')}`,
  );
}

for (const webgl of [false, true]) {
  const renderer = webgl ? 'WebGL' : 'Canvas 2D';
  test(`${renderer} redraws tracks after DPR changes`, async ({page}) => {
    const pth = new PerfettoTestHelper(page);
    const viewport = ensureExists(page.viewportSize());
    const cdp = await page.context().newCDPSession(page);
    const setDpr = (deviceScaleFactor: number) =>
      cdp.send('Emulation.setDeviceMetricsOverride', {
        ...viewport,
        deviceScaleFactor,
        mobile: false,
      });
    await setDpr(1);
    await page.goto('/?testing=1');
    await page.evaluate((enabled) => {
      localStorage.setItem(
        'perfettoFeatureFlags',
        JSON.stringify({
          webglRendering: enabled ? 'OVERRIDE_TRUE' : 'OVERRIDE_FALSE',
        }),
      );
    }, webgl);
    await pth.openTraceFile('slice_double_click_zoom.pb');

    const track = pth
      .locateTrack('Global Track Events/Slices')
      .locator('.pf-track__canvas');
    const clip = ensureExists(await track.boundingBox());
    const selector = '.pf-virtual-overlay-canvas__canvas-container > canvas';
    const trackTree = page.locator('.pf-track-tree', {has: track});
    const canvases = trackTree.locator(selector);
    await expect(canvases).toHaveCount(webgl ? 2 : 1);
    const originalCanvases = await trackTree.evaluateHandle(
      (element, selector) =>
        Array.from(element.querySelectorAll<HTMLCanvasElement>(selector)),
      selector,
    );
    const readCanvases = () =>
      originalCanvases.evaluate((elements) =>
        elements.map((canvas) => {
          const bounds = canvas.getBoundingClientRect();
          const gl = canvas.getContext('webgl2');
          return {
            connected: canvas.isConnected,
            width: canvas.width,
            height: canvas.height,
            cssWidth: bounds.width,
            cssHeight: bounds.height,
            webgl: gl !== null && !gl.isContextLost(),
            viewport: gl
              ? Array.from(gl.getParameter(gl.VIEWPORT) as Int32Array)
              : undefined,
          };
        }),
      );
    const initial = await readCanvases();
    expect(initial.map((canvas) => canvas.webgl)).toEqual(
      webgl ? [true, false] : [false],
    );
    const baseline = await sampleTrack(page, clip);
    expect(baseline[1]).not.toEqual(baseline[0]);

    for (const dpr of [2, 1.25, 1]) {
      await setDpr(dpr);
      await expect
        .poll(() =>
          page.evaluate(() => ({
            width: innerWidth,
            height: innerHeight,
            dpr: devicePixelRatio,
          })),
        )
        .toEqual({...viewport, dpr});
      await expect(canvases).toHaveCount(initial.length);
      const expected = initial.map((canvas) => {
        const width = Math.floor(canvas.cssWidth * dpr);
        const height = Math.floor(canvas.cssHeight * dpr);
        return {
          ...canvas,
          connected: true,
          width,
          height,
          viewport: canvas.webgl ? [0, 0, width, height] : undefined,
        };
      });
      await expect.poll(readCanvases).toEqual(expected);
      await pth.waitForPerfettoIdle();
      expect(await track.boundingBox()).toEqual(clip);
      const pixels = await sampleTrack(page, clip);
      pixels.forEach((pixel, i) =>
        pixel.forEach((channel, j) => {
          expect(Math.abs(channel - baseline[i][j])).toBeLessThanOrEqual(3);
        }),
      );
    }
    await cdp.detach();
  });
}
