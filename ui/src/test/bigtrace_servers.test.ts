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

// Drives the Bigtrace UI's server picker against real local servers
// (tools/bigtrace_server.py), including the local server setup guide, which
// connects once a server answers on the local port (5001):
//   cd ui && BIGTRACE_E2E=1 [BIGTRACE_SHELL=<trace_processor_shell>] \
//     node_modules/.bin/playwright test src/test/bigtrace_servers.test.ts

import {test, type Page, expect as baseExpect} from '@playwright/test';
import {spawn, type ChildProcess} from 'child_process';
import fs from 'fs';
import net from 'net';
import os from 'os';
import path from 'path';

test.skip(
  !['1', 'true'].includes(process.env.BIGTRACE_E2E ?? ''),
  'Set BIGTRACE_E2E=1 to run the Bigtrace live server E2E suite',
);

const expect = baseExpect.configure({timeout: 30_000});
const ROOT = path.resolve(__dirname, '../../..');
const LOCAL_PORT = 5001;
const TRACES = [
  'api24_startup_cold.perfetto-trace',
  'api24_startup_hot.perfetto-trace',
];

let page: Page;
let tracesDir = '';
const servers: ChildProcess[] = [];

function portFree(port: number): Promise<boolean> {
  return new Promise((resolve) => {
    const srv = net.createServer();
    srv.once('error', () => resolve(false));
    srv.listen(port, '127.0.0.1', () => srv.close(() => resolve(true)));
  });
}

function freePort(): Promise<number> {
  return new Promise((resolve, reject) => {
    const srv = net.createServer().listen(0, '127.0.0.1', () => {
      const {port} = srv.address() as net.AddressInfo;
      srv.close(() => resolve(port));
    });
    srv.on('error', reject);
  });
}

// Starts tools/bigtrace_server.py on `port`; resolves once it answers.
async function startServer(port: number): Promise<ChildProcess> {
  let stderr = '';
  const server = spawn(
    process.env.BIGTRACE_PYTHON ?? 'python3',
    [
      path.join(ROOT, 'tools/bigtrace_server.py'),
      'serve',
      tracesDir,
      '-p',
      String(port),
      '--shell-path',
      process.env.BIGTRACE_SHELL ??
        path.join(ROOT, 'out/release/trace_processor_shell'),
    ],
    {stdio: ['ignore', 'ignore', 'pipe']},
  );
  server.stderr?.on('data', (d: Buffer) => {
    stderr = (stderr + d.toString()).slice(-4000);
  });
  servers.push(server);
  await expect
    .poll(
      () =>
        fetch(`http://127.0.0.1:${port}/v1/`)
          .then((r) => r.ok)
          .catch(() => false),
      {message: `server on ${port} did not start:\n${stderr}`},
    )
    .toBe(true);
  return server;
}

async function stopServer(server: ChildProcess) {
  const exited = new Promise((r) => server.once('exit', r));
  server.kill();
  await exited;
}

test.describe('Bigtrace server picker', () => {
  test.describe.configure({mode: 'serial'});

  test.beforeAll(async ({browser}) => {
    test.skip(
      !(await portFree(LOCAL_PORT)),
      `port ${LOCAL_PORT} is busy (is a local Bigtrace server running?)`,
    );
    tracesDir = fs.mkdtempSync(path.join(os.tmpdir(), 'bt_e2e_servers_'));
    for (const t of TRACES) {
      fs.symlinkSync(path.join(ROOT, 'test/data', t), path.join(tracesDir, t));
    }
    page = await browser.newPage();
  });

  test.afterAll(async () => {
    await Promise.all(servers.map((s) => s.exitCode === null && stopServer(s)));
    if (tracesDir) fs.rmSync(tracesDir, {recursive: true, force: true});
    await page?.close();
  });

  test.beforeEach(async () => {
    await page.goto('/bigtrace.html');
    await page.evaluate(() => localStorage.clear());
    await page.goto('/bigtrace.html');
  });

  const panel = () => page.locator('.pf-bt-connection-panel');
  const serverRow = (kind: string, name?: string) =>
    panel()
      .locator(`.pf-bt-server[data-server-kind="${kind}"]`)
      .filter(name ? {hasText: name} : {});
  const topbarButton = () =>
    page.locator('.pf-topbar button:has(i:text-is("cloud_done"))');
  const connectionButton = () =>
    page.locator('.pf-topbar button:has(i:text-matches("^cloud_(done|off)$"))');

  async function openPanel() {
    // The panel opens by itself once the app finds no working backend.
    try {
      await panel().waitFor({timeout: 5000});
    } catch {
      await connectionButton().click();
    }
    await expect(panel()).toBeVisible();
  }

  test('lists the built-in servers, and says the local one is down', async () => {
    await openPanel();
    await expect(serverRow('service')).toContainText('BigTrace service');
    await expect(serverRow('local')).toContainText('Local server');
    await expect(serverRow('local')).toContainText('http://127.0.0.1:5001/v1');
    await expect(
      serverRow('local').locator('.pf-bt-server__dot--offline'),
    ).toBeVisible();
  });

  test('the setup guide connects once the local server is up', async () => {
    await openPanel();
    await serverRow('local').locator('button:has-text("Set up")').click();
    const guide = panel().locator('.pf-bt-local-setup');
    // This page (localhost) is allowed by default: no --cors-origin.
    await expect(guide.locator('code').first()).toHaveText(
      'curl -fsSL https://raw.githubusercontent.com/google/perfetto/main/' +
        'tools/install-bigtrace | python3 -',
    );
    await expect(guide.locator('code').nth(1)).toHaveText(
      'ssh -N -L 5001:127.0.0.1:5001 YOUR_WORKSTATION',
    );
    await expect(guide).toContainText('Waiting for http://127.0.0.1:5001/v1');

    const navigated = page.waitForEvent('framenavigated');
    await startServer(LOCAL_PORT);
    await navigated;
    await expect(topbarButton()).toHaveText(/Local server/);
    expect(
      await page.evaluate(
        () => JSON.parse(localStorage.bigtraceSettings).bigtraceEndpoint,
      ),
    ).toBe('http://127.0.0.1:5001/v1');

    // Once connected, the row shows what the server serves.
    await topbarButton().click();
    await expect(serverRow('local')).toContainText(
      `${TRACES.length} traces in ${fs.realpathSync(tracesDir)}`,
    );
    await expect(serverRow('local')).toHaveClass(/pf-bt-server--active/);
  });

  test('servers can be added, picked and removed', async () => {
    const port = await freePort();
    const endpoint = `http://127.0.0.1:${port}/v1`;
    await startServer(port);
    await openPanel();
    await panel().locator('button:has-text("Add server")').click();
    const form = panel().locator('.pf-bt-server-form');
    await form.locator('input').nth(0).fill('Colleague');
    await form.locator('input').nth(1).fill('not a url');
    await form.locator('button:has-text("Add")').click();
    await expect(form).toContainText('Enter an http:// or https:// URL');
    await form.locator('input').nth(1).fill(`${endpoint}/`);
    await form.locator('button:has-text("Add")').click();

    const row = serverRow('custom', 'Colleague');
    await expect(row).toContainText(`${TRACES.length} traces in`);
    await expect(row.locator('.pf-bt-server__dot--online')).toBeVisible();

    // Picking a server reloads the app on it.
    const navigated = page.waitForEvent('framenavigated');
    await row.click();
    await navigated;
    await expect(topbarButton()).toHaveText(/Colleague/);

    // Survives the reload; removing it takes it off the list.
    await topbarButton().click();
    await expect(serverRow('custom', 'Colleague')).toHaveClass(
      /pf-bt-server--active/,
    );
    await panel().locator('button[title="Remove Colleague"]').click();
    await expect(serverRow('custom', 'Colleague')).toHaveCount(0);
    // The app still talks to it until another server is picked, so it is
    // shown as the unsaved current server.
    await expect(panel()).toContainText('Current server');
  });

  test('a server going down shows up as down', async () => {
    const port = await freePort();
    const server = await startServer(port);
    await openPanel();
    await panel().locator('button:has-text("Add server")').click();
    const form = panel().locator('.pf-bt-server-form');
    await form.locator('input').nth(1).fill(`http://127.0.0.1:${port}/v1`);
    await form.locator('button:has-text("Add")').click();
    const row = serverRow('custom', `127.0.0.1:${port}`);
    await expect(row.locator('.pf-bt-server__dot--online')).toBeVisible();
    await stopServer(server);
    await expect(row.locator('.pf-bt-server__dot--offline')).toBeVisible();
  });
});
