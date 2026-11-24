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

import {z} from 'zod';

// Single-line text: used in `--` SQL comment headers and tab titles, so a line
// break would let the remainder escape the comment.
const ONE_LINE = z
  .string()
  .trim()
  .transform((s) => s.replace(/\s*[\r\n]+\s*/g, ' '));

// Trace list posted by the page that opened this window via postMessage.
const SHARED_TRACE_LIST_SCHEMA = z.object({
  traceUuids: z
    .array(z.string())
    .transform((uuids) => [
      ...new Set(uuids.map((u) => u.trim()).filter((u) => u !== '')),
    ])
    .refine((uuids) => uuids.length > 0),
  sourceUrl: ONE_LINE.optional(),
  title: ONE_LINE.optional(),
  perfettoSql: z.string().trim().optional(),
});

export type SharedTraceList = z.infer<typeof SHARED_TRACE_LIST_SCHEMA>;

export function parseSharedTraceListMessage(
  data: unknown,
): SharedTraceList | undefined {
  // jitless: zod's default object fast path compiles the schema with
  // `new Function`, which BigTrace's CSP (`script-src` without
  // 'unsafe-eval') rejects. zod decides whether eval is available when the
  // schema is built, before the CSP meta tag is installed, so without this
  // the first parse throws an EvalError instead of returning a result.
  const parsed = SHARED_TRACE_LIST_SCHEMA.safeParse(data, {jitless: true});
  return parsed.success ? parsed.data : undefined;
}

export function formatSharedTraceListSql(shared: SharedTraceList): string {
  const lines: string[] = [];
  if (shared.title) lines.push(`-- ${shared.title}`);
  if (shared.sourceUrl) lines.push(`-- Source: ${shared.sourceUrl}`);
  const header = lines.length > 0 ? `${lines.join('\n')}\n\n` : '';
  return shared.perfettoSql ? `${header}${shared.perfettoSql}\n` : header;
}

// Answers the opener's PING with PONG and returns the trace list once posted.
// Messages from anything other than the opener are ignored.
export function handleSharedTraceListPostMessage(
  e: MessageEvent,
): SharedTraceList | undefined {
  if (e.source == null || e.source !== window.opener) return undefined;
  if (e.data === 'PING') {
    e.source.postMessage('PONG', {targetOrigin: e.origin});
    return undefined;
  }
  return parseSharedTraceListMessage(e.data);
}
