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

export function fmtBytes(bytes: number | null | undefined): string {
  if (bytes === null || bytes === undefined || !Number.isFinite(bytes)) {
    return '—';
  }
  const neg = bytes < 0;
  const abs = Math.abs(bytes);
  const sign = neg ? '−' : '';
  if (abs >= 1024 ** 3) return `${sign}${(abs / 1024 ** 3).toFixed(2)} GiB`;
  if (abs >= 1024 ** 2) return `${sign}${(abs / 1024 ** 2).toFixed(1)} MiB`;
  if (abs >= 1024) return `${sign}${(abs / 1024).toFixed(0)} KiB`;
  return `${sign}${Math.round(abs)} B`;
}

export function fmtDeltaBytes(bytes: number | null | undefined): string {
  if (bytes === null || bytes === undefined || !Number.isFinite(bytes)) {
    return '—';
  }
  if (Math.abs(bytes) < 1024) return '0 B';
  return bytes > 0 ? `+${fmtBytes(bytes)}` : fmtBytes(bytes);
}

export function fmtRateMb(mbPerSec: number | null | undefined): string {
  if (mbPerSec === null || mbPerSec === undefined || !Number.isFinite(mbPerSec)) {
    return '—';
  }
  if (mbPerSec >= 100) return `${mbPerSec.toFixed(0)} MB/s`;
  if (mbPerSec >= 1) return `${mbPerSec.toFixed(1)} MB/s`;
  if (mbPerSec > 0) return `${(mbPerSec * 1024).toFixed(0)} KB/s`;
  return '0 MB/s';
}

export function fmtPct(pct: number | null | undefined, digits = 1): string {
  if (pct === null || pct === undefined || !Number.isFinite(pct)) return '—';
  if (pct > 0 && pct < 0.1) return '<0.1%';
  return `${pct.toFixed(digits)}%`;
}

export function fmtDeltaPct(pct: number | null | undefined): string {
  if (pct === null || pct === undefined || !Number.isFinite(pct)) return '—';
  if (Math.abs(pct) < 0.1) return '0%';
  const sign = pct > 0 ? '+' : '−';
  return `${sign}${Math.abs(pct).toFixed(1)}%`;
}

export function fmtMs(ms: number | null | undefined): string {
  if (ms === null || ms === undefined || !Number.isFinite(ms)) return '—';
  if (ms >= 1000) return `${(ms / 1000).toFixed(2)} s`;
  if (ms >= 10) return `${ms.toFixed(0)} ms`;
  if (ms >= 1) return `${ms.toFixed(1)} ms`;
  if (ms > 0) return `${(ms * 1000).toFixed(0)} µs`;
  return '0 ms';
}

export function fmtCount(n: number | null | undefined): string {
  if (n === null || n === undefined || !Number.isFinite(n)) return '—';
  if (Math.abs(n) >= 1e6) return `${(n / 1e6).toFixed(1)}M`;
  if (Math.abs(n) >= 1e3) return `${(n / 1e3).toFixed(1)}k`;
  return `${Math.round(n)}`;
}

export function fmtFreqMhz(khz: number | null | undefined): string {
  if (khz === null || khz === undefined || !Number.isFinite(khz)) return '—';
  const mhz = khz / 1000;
  if (mhz >= 1000) return `${(mhz / 1000).toFixed(2)} GHz`;
  return `${Math.round(mhz)} MHz`;
}

export function fmtTempC(val: number | null | undefined): string {
  if (val === null || val === undefined || !Number.isFinite(val)) return '—';
  // Linux thermal zones report either °C or milli-°C depending on kernel/parser.
  const c = val > 200 ? val / 1000 : val;
  return `${c.toFixed(1)} °C`;
}

export function fmtPowerMw(mw: number | null | undefined): string {
  if (mw === null || mw === undefined || !Number.isFinite(mw)) return '—';
  if (Math.abs(mw) >= 1000) return `${(mw / 1000).toFixed(2)} W`;
  return `${Math.round(mw)} mW`;
}

export function fmtClockTime(ms: number): string {
  const d = new Date(ms);
  const pad = (x: number) => String(x).padStart(2, '0');
  return `${pad(d.getHours())}:${pad(d.getMinutes())}:${pad(d.getSeconds())}`;
}
