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

export function fmtDur(ns: number): string {
  if (ns >= 1e9) return (ns / 1e9).toFixed(3) + ' s';
  if (ns >= 1e6) return (ns / 1e6).toFixed(1) + ' ms';
  return (ns / 1e3).toFixed(0) + ' \u00b5s';
}

export function fmtPct(ns: number, total: number): string {
  if (total === 0) return '0.0%';
  return ((ns / total) * 100).toFixed(1) + '%';
}
