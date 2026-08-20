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

import {readFileSync} from 'fs';
import {expect, type Page, test} from '@playwright/test';
import {PerfettoTestHelper} from './perfetto_ui_test_helper';

// Structural (screenshot-free) coverage of the aggregate-profiles merge page:
// a tar archive of the two checked-in pprofs is assembled in the test and
// opened as a buffer, so no dedicated GCS-hosted archive is needed.

// Minimal ustar writer: enough for regular files with short names.
function tarHeader(name: string, size: number): Buffer {
  const header = Buffer.alloc(512);
  header.write(name, 0, 'ascii'); // name
  header.write('0000644\0', 100, 'ascii'); // mode
  header.write('0000000\0', 108, 'ascii'); // uid
  header.write('0000000\0', 116, 'ascii'); // gid
  header.write(size.toString(8).padStart(11, '0') + '\0', 124, 'ascii');
  header.write('00000000000\0', 136, 'ascii'); // mtime
  header.write('        ', 148, 'ascii'); // chksum placeholder
  header.write('0', 156, 'ascii'); // typeflag: regular file
  header.write('ustar\0' + '00', 257, 'ascii'); // magic + version
  let sum = 0;
  for (const byte of header) {
    sum += byte;
  }
  header.write(sum.toString(8).padStart(6, '0') + '\0 ', 148, 'ascii');
  return header;
}

function buildTar(
  members: ReadonlyArray<{name: string; data: Buffer}>,
): Buffer {
  const blocks: Buffer[] = [];
  for (const {name, data} of members) {
    blocks.push(tarHeader(name, data.length));
    blocks.push(data);
    const pad = (512 - (data.length % 512)) % 512;
    if (pad > 0) {
      blocks.push(Buffer.alloc(pad));
    }
  }
  blocks.push(Buffer.alloc(1024)); // end-of-archive
  return Buffer.concat(blocks);
}

test.describe.configure({mode: 'serial'});

let pth: PerfettoTestHelper;
let page: Page;

test.beforeAll(async ({browser}) => {
  page = await browser.newPage();
  pth = new PerfettoTestHelper(page);
  const archive = buildTar([
    {
      name: 'a.pprof',
      data: readFileSync(pth.getTestTracePath('pprof_simple_cpu.pprof')),
    },
    {
      name: 'b.pprof',
      data: readFileSync(pth.getTestTracePath('pprof_multi_metric.pprof')),
    },
  ]);
  await pth.openTraceBuffer('profiles.tar', archive);
});

test('lands on the merge page for a profile-only archive', async () => {
  // A pprof-only trace has no timeline data, so the plugin suggests the
  // aggregate profiles page as the initial page.
  expect(page.url()).toContain('/aggregateprofiles');
  await expect(page.locator('.pf-flamegraph-collection')).toBeVisible();
});

test('grid lists one row per profile with sample-type columns', async () => {
  const grid = page.locator('.pf-flamegraph-collection__grid');
  await expect(grid).toBeVisible();
  await expect(grid).toContainText('profile');
  await expect(grid).toContainText('cpu (nanoseconds)');
  await expect(grid).toContainText('allocations (count)');
  await expect(grid).toContainText('a.pprof');
  await expect(grid).toContainText('b.pprof');
});

test('column menus offer no grid-only grouping', async () => {
  // Pivoting would regroup the grid without the flamegraph following, so
  // the collection disables DataGrid's pivot controls.
  const header = page.getByRole('columnheader', {name: /^profile/});
  await header.hover();
  await header.getByRole('button', {name: 'Column menu'}).click();
  const menu = page.locator('.pf-popup-content');
  await expect(menu).toContainText('Fit to content');
  await expect(menu).not.toContainText('Group by this column');
  await page.keyboard.press('Escape');
});

test('merges the working set into one flamegraph by default', async () => {
  await expect(page.locator('.pf-flamegraph-collection__summary')).toHaveText(
    'Merging 2 of 2 profiles',
  );
  await expect(page.locator('.pf-flamegraph')).toBeVisible();
});

test('merge off steps through the profiles', async () => {
  await page
    .locator('.pf-flamegraph-collection__flame-head label', {
      hasText: 'Merge',
    })
    .click();
  await pth.waitForPerfettoIdle();
  await expect(page.locator('.pf-flamegraph-collection__summary')).toHaveText(
    'Showing 2 of 2 profiles',
  );
  await expect(page.locator('.pf-flamegraph-collection__flame-pos')).toHaveText(
    '1 / 2',
  );
  await expect(
    page.locator('.pf-flamegraph-collection__flame-cell-title'),
  ).toHaveText('a.pprof');

  // Chevron to the next profile.
  await page
    .locator('.pf-flamegraph-collection__flame-nav button')
    .last()
    .click();
  await pth.waitForPerfettoIdle();
  await expect(page.locator('.pf-flamegraph-collection__flame-pos')).toHaveText(
    '2 / 2',
  );
  await expect(
    page.locator('.pf-flamegraph-collection__flame-cell-title'),
  ).toHaveText('b.pprof');

  // Arrow keys step too.
  await page.keyboard.press('ArrowLeft');
  await pth.waitForPerfettoIdle();
  await expect(page.locator('.pf-flamegraph-collection__flame-pos')).toHaveText(
    '1 / 2',
  );
});

test('merge back on restores the combined flamegraph', async () => {
  await page
    .locator('.pf-flamegraph-collection__flame-head label', {
      hasText: 'Merge',
    })
    .click();
  await pth.waitForPerfettoIdle();
  await expect(page.locator('.pf-flamegraph-collection__summary')).toHaveText(
    'Merging 2 of 2 profiles',
  );
  await expect(page.locator('.pf-flamegraph')).toBeVisible();
});
