// Copyright (C) 2026 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Engine startup benchmark page — see bench.html.

(function () {
  'use strict';

  const WORKER_URL = 'engine_bench_bundle.js';
  // ?precompile=0 forces each worker to fetch + compile its own copy.
  const PRECOMPILE =
    new URLSearchParams(location.search).get('precompile') !== '0';
  // The bench requires a memory64-capable browser (Chromium ≥ 133,
  // Firefox ≥ 134). No fallback.
  const WASM_URL = 'trace_processor_memory64.wasm';
  const compiledModulePromise = PRECOMPILE
    ? WebAssembly.compileStreaming(fetch(WASM_URL))
    : Promise.resolve(undefined);

  const $ = (id) => document.getElementById(id);
  const params = new URLSearchParams(location.search);
  const initialN = parseInt(params.get('n') || '', 10);
  const initialWarmup = parseInt(params.get('warmup') || '', 10);
  const initialCold = params.get('cache') === 'cold';
  if (!isNaN(initialN)) $('n').value = String(initialN);
  if (!isNaN(initialWarmup)) $('warmup').value = String(initialWarmup);
  if (initialCold) $('cold').checked = true;

  function log(msg) {
    const el = $('log');
    el.textContent += msg + '\n';
    el.scrollTop = el.scrollHeight;
    console.log('[bench]', msg);
  }

  async function runOne(cacheBust) {
    const url = cacheBust !== '' ? `${WORKER_URL}?cb=${cacheBust}` : WORKER_URL;
    const t0 = performance.now();
    const worker = new Worker(url);
    const tAfterCtor = performance.now();
    const wasmModule = await compiledModulePromise;
    const bootstrap = {useMemory64: true};
    if (wasmModule) bootstrap.wasmModule = wasmModule;
    worker.postMessage(bootstrap);
    try {
      return await new Promise((resolve, reject) => {
        const timeout = setTimeout(
          () => reject(new Error('worker did not report bench-marks in 30s')),
          30000,
        );
        worker.onerror = (e) => {
          clearTimeout(timeout);
          reject(new Error(`worker error: ${e.message || e.type}`));
        };
        worker.onmessage = (msg) => {
          if (msg.data === undefined || msg.data === null) return;
          if (msg.data.type === 'bench-error') {
            clearTimeout(timeout);
            reject(new Error(`worker reported error: ${msg.data.message}`));
            return;
          }
          if (msg.data.type !== 'bench-marks') return;
          clearTimeout(timeout);
          const tReceived = performance.now();
          resolve({
            new_worker_sync_ms: tAfterCtor - t0,
            e2e_main_thread_ms: tReceived - t0,
            ...msg.data.phases,
          });
        };
      });
    } finally {
      worker.terminate();
    }
  }

  function percentile(sorted, p) {
    if (sorted.length === 0) return NaN;
    const idx = Math.min(
      sorted.length - 1,
      Math.max(0, Math.floor((p / 100) * sorted.length)),
    );
    return sorted[idx];
  }

  function summarise(samples) {
    if (samples.length === 0) return {};
    const keys = Object.keys(samples[0]);
    const out = {};
    for (const k of keys) {
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

  function renderTable(summary) {
    const tbody = $('results').querySelector('tbody');
    tbody.innerHTML = '';
    const order = [
      'e2e_main_thread_ms',
      'new_worker_sync_ms',
      'bundle_eval_ms',
      'bridge_ctor_ms',
      'start_init_sync_ms',
      'init_async_ms',
      'total_worker_ms',
    ];
    const fmt = (v) => v.toFixed(1);
    for (const k of order) {
      if (!(k in summary)) continue;
      const s = summary[k];
      const tr = document.createElement('tr');
      tr.innerHTML =
        `<td>${k}</td><td>${fmt(s.min)}</td><td>${fmt(s.p50)}</td>` +
        `<td>${fmt(s.p95)}</td><td>${fmt(s.max)}</td><td>${fmt(s.mean)}</td>`;
      tbody.appendChild(tr);
    }
    $('results').style.display = '';
  }

  async function run() {
    $('run').disabled = true;
    const n = parseInt($('n').value, 10);
    const warmup = parseInt($('warmup').value, 10);
    const cold = $('cold').checked;
    log(`Starting: n=${n}, warmup=${warmup}, cold=${cold}`);
    const samples = [];
    // Sequential: parallel spawns contend for the wasm code-cache slot.
    for (let i = 0; i < warmup + n; i++) {
      const cacheBust = cold === true ? `${Date.now()}-${i}` : '';
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
        log(`ERROR at iter ${i}: ${e.message}`);
        $('status').textContent = `Aborted at iter ${i}: ${e.message}`;
        $('run').disabled = false;
        return;
      }
    }
    const summary = summarise(samples);
    renderTable(summary);
    $('status').textContent = `Done. ${samples.length} samples.`;
    window.__benchResults = {config: {n, warmup, cold}, samples, summary};
    log('Results stored on window.__benchResults');
    $('run').disabled = false;
  }

  $('run').addEventListener('click', run);
  if (params.get('auto') === '1') {
    setTimeout(run, 0);
  }
})();
