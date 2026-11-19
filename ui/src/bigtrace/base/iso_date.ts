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

// Calendar dates as 'YYYY-MM-DD' strings: they compare as strings and carry
// no timezone, which is what date-typed columns and date pickers want.

export interface YearMonth {
  readonly year: number;
  readonly month: number; // 0-based
}

// An inclusive span of days; either end may be open.
export interface DateRange {
  readonly from?: string;
  readonly to?: string;
}

const MS_PER_DAY = 86_400_000;

function pad2(n: number): string {
  return n < 10 ? `0${n}` : `${n}`;
}

function toUtc(iso: string): Date {
  const [y, mo, d] = iso.split('-').map(Number);
  return new Date(Date.UTC(y!, mo! - 1, d));
}

export function isoDate(year: number, month: number, day: number): string {
  return `${year}-${pad2(month + 1)}-${pad2(day)}`;
}

// Today in the browser's local calendar.
export function todayIso(now: Date = new Date()): string {
  return isoDate(now.getFullYear(), now.getMonth(), now.getDate());
}

export function addDays(iso: string, days: number): string {
  const d = new Date(toUtc(iso).getTime() + days * MS_PER_DAY);
  return isoDate(d.getUTCFullYear(), d.getUTCMonth(), d.getUTCDate());
}

// 'Sep 29'
export function formatShortDate(iso: string): string {
  return toUtc(iso).toLocaleDateString('en-US', {
    month: 'short',
    day: 'numeric',
    timeZone: 'UTC',
  });
}

// 'September 29, 2026' — the full day for accessible names.
export function formatLongDate(iso: string): string {
  return toUtc(iso).toLocaleDateString('en-US', {
    month: 'long',
    day: 'numeric',
    year: 'numeric',
    timeZone: 'UTC',
  });
}

// 'September 2026'
export function formatMonth(ym: YearMonth): string {
  return new Date(Date.UTC(ym.year, ym.month, 1)).toLocaleDateString('en-US', {
    month: 'long',
    year: 'numeric',
    timeZone: 'UTC',
  });
}

export function yearMonthOf(iso: string): YearMonth {
  const [y, mo] = iso.split('-').map(Number);
  return {year: y!, month: mo! - 1};
}

export function shiftMonth(ym: YearMonth, by: number): YearMonth {
  const d = new Date(Date.UTC(ym.year, ym.month + by, 1));
  return {year: d.getUTCFullYear(), month: d.getUTCMonth()};
}

export function daysInMonth(ym: YearMonth): number {
  return new Date(Date.UTC(ym.year, ym.month + 1, 0)).getUTCDate();
}

// 0 = Monday ... 6 = Sunday.
export function mondayBasedWeekday(iso: string): number {
  return (toUtc(iso).getUTCDay() + 6) % 7;
}
