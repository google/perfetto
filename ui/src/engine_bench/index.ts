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

// Engine startup benchmark page driver — see bench.html.

const WORKER_URL = 'engine_bench_worker_bundle.js';
const WASM_URL = 'trace_processor_memory64.wasm';
const PHASE_ORDER = [
  'e2e_main_thread_ms',
  'new_worker_sync_ms',
  'bundle_eval_ms',
  'bridge_ctor_ms',
  'start_init_sync_ms',
  'init_async_ms',
  'total_worker_ms',
] as const;
type Phase = (typeof PHASE_ORDER)[number];
type Sample = Record<Phase, number>;
interface Stats {
  min: number;
  p50: number;
  p95: number;
  max: number;
  mean: number;
}

const params = new URLSearchParams(location.search);
// ?precompile=0 forces each worker to fetch + compile its own copy.
const compiledModulePromise: Promise<WebAssembly.Module | undefined> =
  params.get('precompile') === '0'
    ? Promise.resolve(undefined)
    : WebAssembly.compileStreaming(fetch(WASM_URL));

function $<T extends HTMLElement>(id: string): T {
  return document.getElementById(id) as T;
}

function setIfNumber(id: string, raw: string | null): void {
  const n = parseInt(raw ?? '', 10);
  if (!Number.isNaN(n)) $<HTMLInputElement>(id).value = String(n);
}
setIfNumber('n', params.get('n'));
setIfNumber('warmup', params.get('warmup'));
if (params.get('cache') === 'cold') $<HTMLInputElement>('cold').checked = true;

function log(msg: string): void {
  const el = $('log');
  el.textContent += msg + '\n';
  el.scrollTop = el.scrollHeight;
  console.log('[bench]', msg);
}

async function runOne(cacheBust: string): Promise<Sample> {
  const url = cacheBust ? `${WORKER_URL}?cb=${cacheBust}` : WORKER_URL;
  const t0 = performance.now();
  const worker = new Worker(url);
  const tAfterCtor = performance.now();
  const wasmModule = await compiledModulePromise;
  worker.postMessage({useMemory64: true, wasmModule});
  try {
    return await new Promise<Sample>((resolve, reject) => {
      const timeout = setTimeout(
        () => reject(new Error('worker did not report bench-marks in 30s')),
        30000,
      );
      worker.onerror = (e) => {
        clearTimeout(timeout);
        reject(new Error(`worker error: ${e.message || e.type}`));
      };
      worker.onmessage = (msg) => {
        const data = msg.data as
          | {type: 'bench-marks'; phases: Sample}
          | {type: 'bench-error'; message: string}
          | undefined
          | null;
        if (!data) return;
        clearTimeout(timeout);
        if (data.type === 'bench-error') {
          reject(new Error(`worker reported error: ${data.message}`));
          return;
        }
        const tReceived = performance.now();
        resolve({
          ...data.phases,
          new_worker_sync_ms: tAfterCtor - t0,
          e2e_main_thread_ms: tReceived - t0,
        });
      };
    });
  } finally {
    worker.terminate();
  }
}

function percentile(sorted: number[], p: number): number {
  const idx = Math.min(
    sorted.length - 1,
    Math.max(0, Math.floor((p / 100) * sorted.length)),
  );
  return sorted[idx];
}

function summarise(samples: Sample[]): Partial<Record<Phase, Stats>> {
  const out: Partial<Record<Phase, Stats>> = {};
  if (samples.length === 0) return out;
  for (const k of Object.keys(samples[0]) as Phase[]) {
    const vals = samples.map((s) => s[k]).sort((a, b) => a - b);
    out[k] = {
      min: vals[0],
      p50: percentile(vals, 50),
      p95: percentile(vals, 95),
      max: vals[vals.length - 1],
      mean: vals.reduce((a, b) => a + b, 0) / vals.length,
    };
  }
  return out;
}

function renderTable(summary: Partial<Record<Phase, Stats>>): void {
  const tbody = $('results').querySelector('tbody')!;
  tbody.innerHTML = '';
  const fmt = (v: number): string => v.toFixed(1);
  for (const k of PHASE_ORDER) {
    const s = summary[k];
    if (!s) continue;
    const tr = document.createElement('tr');
    tr.innerHTML =
      `<td>${k}</td><td>${fmt(s.min)}</td><td>${fmt(s.p50)}</td>` +
      `<td>${fmt(s.p95)}</td><td>${fmt(s.max)}</td><td>${fmt(s.mean)}</td>`;
    tbody.appendChild(tr);
  }
  $('results').style.display = '';
}

async function run(): Promise<void> {
  const runBtn = $<HTMLButtonElement>('run');
  runBtn.disabled = true;
  const n = parseInt($<HTMLInputElement>('n').value, 10);
  const warmup = parseInt($<HTMLInputElement>('warmup').value, 10);
  const cold = $<HTMLInputElement>('cold').checked;
  log(`Starting: n=${n}, warmup=${warmup}, cold=${cold}`);
  const samples: Sample[] = [];
  // Sequential: parallel spawns contend for the wasm code-cache slot.
  for (let i = 0; i < warmup + n; i++) {
    const cacheBust = cold ? `${Date.now()}-${i}` : '';
    try {
      const s = await runOne(cacheBust);
      const tag = i < warmup ? 'warmup' : `iter ${i - warmup + 1}/${n}`;
      log(
        `${tag}: e2e=${s.e2e_main_thread_ms.toFixed(1)}ms ` +
          `start_init=${s.start_init_sync_ms.toFixed(1)}ms ` +
          `init_async=${s.init_async_ms.toFixed(1)}ms`,
      );
      if (i >= warmup) samples.push(s);
      $('status').textContent = `Running… ${i + 1}/${warmup + n}`;
    } catch (e) {
      const msg = e instanceof Error ? e.message : String(e);
      log(`ERROR at iter ${i}: ${msg}`);
      $('status').textContent = `Aborted at iter ${i}: ${msg}`;
      runBtn.disabled = false;
      return;
    }
  }
  const summary = summarise(samples);
  renderTable(summary);
  $('status').textContent = `Done. ${samples.length} samples.`;
  (window as unknown as {__benchResults: unknown}).__benchResults = {
    config: {n, warmup, cold},
    samples,
    summary,
  };
  log('Results stored on window.__benchResults');
  runBtn.disabled = false;
}

$('run').addEventListener('click', run);
if (params.get('auto') === '1') setTimeout(run, 0);
