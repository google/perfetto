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

// Evidence chips: the model backs each claim with an inline citation written
// as [[...]], which the chat panel renders as a clickable chip. Three kinds:
//
//   [[Q3]]                   A query the model ran with run_query. Every
//                            successful run_query result is given an id
//                            (Q1, Q2, ...) which the model cites. Clicking
//                            opens that exact query in the Query page.
//   [[slice 1234]]           A single event, as a SQL table + row id. Clicking
//                            selects it on the timeline.
//   [[range 1000 2000]]      A time range in trace-processor ns. Clicking pans
//                            the timeline to it.
//
// Query evidence is only citable after the query has actually run, so numbers
// in the answer can always be traced back to the SQL that produced them.

export type Evidence =
  | {readonly kind: 'query'; readonly id: string}
  | {readonly kind: 'event'; readonly table: string; readonly id: number}
  | {readonly kind: 'range'; readonly start: bigint; readonly end: bigint};

const QUERY_RE = /^Q\d+$/;
const EVENT_RE = /^([a-z_][a-z0-9_]*) (\d+)$/;
const RANGE_RE = /^range (\d+) (\d+)$/;

/**
 * Parse the inside of a [[...]] citation. Returns undefined if it isn't a
 * well-formed citation (in which case it is left as plain text).
 */
export function parseEvidence(text: string): Evidence | undefined {
  const s = text.trim().replace(/\s+/g, ' ');
  if (QUERY_RE.test(s)) {
    return {kind: 'query', id: s};
  }
  const range = RANGE_RE.exec(s);
  if (range !== null) {
    const [, start = '0', end = '0'] = range;
    const startNs = BigInt(start);
    const endNs = BigInt(end);
    if (startNs > endNs) return undefined;
    return {kind: 'range', start: startNs, end: endNs};
  }
  const event = EVENT_RE.exec(s);
  if (event !== null) {
    const [, table = '', id = '0'] = event;
    return {kind: 'event', table, id: Number(id)};
  }
  return undefined;
}

/** Short label shown on a chip. */
export function evidenceLabel(ev: Evidence): string {
  switch (ev.kind) {
    case 'query':
      return ev.id;
    case 'event':
      return `${ev.table} ${ev.id}`;
    case 'range':
      return 'time range';
  }
}

/** Tooltip shown on hover. */
export function evidenceTooltip(ev: Evidence): string {
  switch (ev.kind) {
    case 'query':
      return `Open query ${ev.id} in the Query page`;
    case 'event':
      return `Select ${ev.table} row ${ev.id} on the timeline`;
    case 'range':
      return `Show ${ev.start}-${ev.end} ns on the timeline`;
  }
}

/**
 * The queries the model has run this trace, by evidence id. Trace-scoped and
 * append-only, so ids stay valid across new chat sessions.
 */
export class EvidenceStore {
  private readonly queries = new Map<string, string>();

  /** Record a successfully-run query and return its citable id. */
  addQuery(sql: string): string {
    const id = `Q${this.queries.size + 1}`;
    this.queries.set(id, sql);
    return id;
  }

  getQuery(id: string): string | undefined {
    return this.queries.get(id);
  }
}

/**
 * The evidence rules folded into the system prompt. Kept next to the parser so
 * the syntax the model is told to use and the syntax we accept can't drift.
 */
export const EVIDENCE_PROMPT = `
Evidence: back EVERY factual claim about the trace with at least one evidence
chip, placed right after the claim. Chips are written in double square brackets
and render as clickable links for the user:
- [[Q3]] - a query you ran. Each successful run_query result starts with its
  evidence id; cite that id for any number, list or finding derived from it.
  Never invent ids; only cite ones run_query gave you.
- [[slice 1234]] - one specific event, as \`<sql table> <row id>\` (e.g.
  [[thread_state 99]]). Use for "this slice", "the longest frame" etc.
- [[range 1000 2000]] - a time range, start and end in trace-processor ns.
Example: "The main thread was blocked for 182 ms [[Q2]], mostly in one binder
transaction [[slice 4521]] [[range 1200000 1382000]]."
If you cannot back a claim with evidence, say it is a guess.
`.trim();
