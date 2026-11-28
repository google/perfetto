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

// Automatic conversation compaction: summarizes older agent history when token
// usage approaches the model's context window (or after a context-length
// error), while leaving the user-visible chat transcript intact.

import type {LlmGateway, ModelPath} from '../dev.perfetto.Llm/gateway';
import type {Message} from '../dev.perfetto.Llm/protocol';

// Trigger proactive compaction once the latest reported token count reaches
// this fraction of the model's context window.
export const COMPACTION_THRESHOLD = 0.8;

// Default absolute token ceiling for proactive compaction. Prevents huge-window
// models (e.g. 1M tokens) from growing to multi-MB requests before compacting.
export const DEFAULT_MAX_COMPACTION_TOKENS = 150_000;

// Fallback and upper bound for the serialized transcript handed to the
// summarizer so the compaction call itself stays fast and bounded even on very
// large context windows.
const DEFAULT_TRANSCRIPT_CHAR_LIMIT = 120_000;
const MAX_TRANSCRIPT_CHAR_LIMIT = 400_000;
const MIN_TRANSCRIPT_CHAR_LIMIT = 4_000;
const CHARS_PER_TOKEN_ESTIMATE = 4;

// When deciding whether to keep the current user turn intact during a long
// tool loop, require the older prefix to represent at least this share of the
// history; otherwise split inside the active tool loop so compaction actually
// frees meaningful context.
const MIN_PREFIX_SHARE = 0.25;

// Cap individual tool results in the compaction transcript so one huge SQL
// dump cannot crowd out the rest of the conversation.
const MAX_TOOL_RESULT_CHARS = 4_000;
const MAX_TOOL_ARGS_CHARS = 1_000;

const COMPACTION_SUMMARY_PREFIX =
  'Summary of the earlier conversation (automatically compacted to stay ' +
  'within the context window):';

const COMPACTION_SUMMARY_ACK =
  'Understood. I will continue from this summary of the earlier conversation.';

const COMPACTION_SYSTEM_PROMPT = `
You are summarizing a Perfetto trace analysis conversation so it can continue
within a smaller context window.

Produce a concise, structured summary that preserves everything needed to
continue seamlessly:
- The user's goal, questions asked, and any constraints or preferences.
- Trace context: which trace(s) were inspected, key process/thread names,
  important timestamps or time ranges, and SQL tables/modules used.
- Concrete findings, metrics, root causes, and conclusions reached so far.
- Evidence query IDs (e.g. Q1, Q2) and the key SQL queries or results they
  correspond to, if any were cited.
- Any unfinished investigation steps or open questions.

Be factual and compact. Do not invent details that were not in the transcript.
`.trim();

export interface CompactionSplit {
  readonly toCompact: readonly Message[];
  readonly tail: readonly Message[];
}

export interface CompactionResult {
  readonly history: Message[];
  readonly summary: string;
}

export interface CompactHistoryOptions {
  // 'auto' keeps the active/recent tail so an in-flight turn can continue.
  // 'manual' summarizes an entire completed conversation into a summary/ack
  // pair when idle (or falls back to split compaction if a turn is pending).
  readonly mode?: 'auto' | 'manual';
}

/**
 * True when `history` is already just a compacted summary/ack pair with no
 * newer turns added yet.
 */
export function isCompactedSummary(history: readonly Message[]): boolean {
  return (
    history.length === 2 &&
    history[0]?.role === 'user' &&
    history[0].text.startsWith(COMPACTION_SUMMARY_PREFIX) &&
    history[1]?.role === 'model' &&
    history[1].text === COMPACTION_SUMMARY_ACK
  );
}

/**
 * True when `history` has enough content to be compacted (and isn't already a
 * freshly compacted summary with no subsequent turns).
 */
export function canCompactHistory(history: readonly Message[]): boolean {
  return history.length >= 2 && !isCompactedSummary(history);
}

/**
 * Assemble a post-compaction history for an idle completed conversation,
 * leaving it ready for the user's next message.
 */
export function buildIdleCompactedHistory(summary: string): Message[] {
  const summaryMessage = `${COMPACTION_SUMMARY_PREFIX}\n\n${summary.trim()}`;
  return [
    {role: 'user', text: summaryMessage},
    {role: 'model', text: COMPACTION_SUMMARY_ACK},
  ];
}

/**
 * Split agent history into an older prefix to summarize and a recent tail to
 * keep verbatim. Never splits a tool-call from its matching tool-result.
 * Returns undefined when the history is too short to compact.
 */
export function splitHistoryForCompaction(
  history: readonly Message[],
): CompactionSplit | undefined {
  if (history.length <= 1) return undefined;

  const totalChars = sumChars(history);
  const lastUserIdx = findLastIndex(history, (m) => m.role === 'user');

  // Prefer keeping the entire active user turn verbatim if there is meaningful
  // older history before it (or if we are at the start of a fresh user turn).
  if (lastUserIdx > 0) {
    const prefix = history.slice(0, lastUserIdx);
    const isFreshTurn = lastUserIdx === history.length - 1;
    if (isFreshTurn || sumChars(prefix) >= totalChars * MIN_PREFIX_SHARE) {
      return {toCompact: prefix, tail: history.slice(lastUserIdx)};
    }
  }

  // Mid-turn inside a long tool loop: keep the most recent tool-call +
  // tool-result round verbatim if compacting the prefix before it reclaims a
  // meaningful fraction of the history.
  const lastToolCallIdx = findLastIndex(history, (m) => m.role === 'tool-call');
  if (lastToolCallIdx > 0) {
    const prefix = history.slice(0, lastToolCallIdx);
    if (sumChars(prefix) >= totalChars * MIN_PREFIX_SHARE) {
      return {toCompact: prefix, tail: history.slice(lastToolCallIdx)};
    }
  }

  // Extreme fallback (e.g. a single tool round produced an oversized result):
  // compact everything so the model can continue from the summary.
  return {toCompact: history, tail: []};
}

/**
 * Assemble the post-compaction message history while maintaining valid role
 * alternation for all protocols (Gemini, OpenAI, Chrome Prompt).
 */
export function buildCompactedHistory(
  summary: string,
  tail: readonly Message[],
): Message[] {
  const trimmed = summary.trim();
  const summaryMessage = `${COMPACTION_SUMMARY_PREFIX}\n\n${trimmed}`;
  const firstTail = tail[0];

  if (firstTail === undefined) {
    return [
      {
        role: 'user',
        text: `${summaryMessage}\n\nPlease continue addressing the user's request from where you left off.`,
      },
    ];
  }

  // Insert an assistant acknowledgment when the kept tail starts with a user
  // message so backends requiring alternating user/model turns stay valid.
  if (firstTail.role === 'user') {
    return [
      {role: 'user', text: summaryMessage},
      {role: 'model', text: COMPACTION_SUMMARY_ACK},
      ...tail,
    ];
  }

  // Tail starts with 'model' or 'tool-call' (both map to model/assistant role).
  return [{role: 'user', text: summaryMessage}, ...tail];
}

/**
 * Compute the effective token threshold for proactive compaction: the smaller
 * of 80% of the model's context window and `maxCompactionTokens` (when > 0).
 * Returns undefined when neither bound is configured.
 */
export function compactionTriggerTokens(
  contextWindow: number | undefined,
  maxCompactionTokens: number | undefined,
): number | undefined {
  const windowLimit =
    contextWindow !== undefined && contextWindow > 0
      ? contextWindow * COMPACTION_THRESHOLD
      : undefined;
  const capLimit =
    maxCompactionTokens !== undefined && maxCompactionTokens > 0
      ? maxCompactionTokens
      : undefined;
  if (windowLimit !== undefined && capLimit !== undefined) {
    return Math.min(windowLimit, capLimit);
  }
  return windowLimit ?? capLimit;
}

/**
 * Compute a safe character budget for the summarization input so the
 * compaction request itself fits comfortably inside the model's context window
 * without growing unboundedly on very large windows.
 */
export function transcriptCharBudget(
  contextWindow: number | undefined,
): number {
  if (contextWindow === undefined || contextWindow <= 0) {
    return DEFAULT_TRANSCRIPT_CHAR_LIMIT;
  }
  // Target ~50% of the model's context window for the input transcript, clamped
  // so huge-window models (e.g. 1M tokens) still keep compaction fast.
  return Math.min(
    MAX_TRANSCRIPT_CHAR_LIMIT,
    Math.max(
      MIN_TRANSCRIPT_CHAR_LIMIT,
      Math.floor(contextWindow * 0.5 * CHARS_PER_TOKEN_ESTIMATE),
    ),
  );
}

/**
 * Render a slice of conversation history into a plain-text transcript bounded
 * by `maxChars`, ready to send to the model for summarization without needing
 * tool declarations.
 */
export function serializeMessagesForCompaction(
  messages: readonly Message[],
  maxChars: number,
): string {
  const sections: string[] = [];
  for (const msg of messages) {
    switch (msg.role) {
      case 'user':
        sections.push(`User:\n${msg.text}`);
        break;
      case 'model':
        sections.push(`Assistant:\n${msg.text}`);
        break;
      case 'tool-call':
        for (const call of msg.calls) {
          const args = truncateText(safeJson(call.args), MAX_TOOL_ARGS_CHARS);
          sections.push(`Tool call ${call.name}(${args})`);
        }
        break;
      case 'tool-result':
        for (const res of msg.results) {
          const status = res.isError === true ? ' [error]' : '';
          const body = truncateText(res.result, MAX_TOOL_RESULT_CHARS);
          sections.push(`Tool result ${res.name}${status}:\n${body}`);
        }
        break;
    }
  }

  const full = sections.join('\n\n');
  if (full.length <= maxChars) return full;

  // Preserve both the opening context (25%) and the most recent turns (75%).
  const omittedNotice =
    '\n\n[... earlier transcript omitted for length ...]\n\n';
  const available = Math.max(0, maxChars - omittedNotice.length);
  const headChars = Math.floor(available * 0.25);
  const tailChars = available - headChars;
  return (
    full.slice(0, headChars) +
    omittedNotice +
    (tailChars > 0 ? full.slice(full.length - tailChars) : '')
  );
}

/**
 * Summarize the older portion of `history` using `model` and return the
 * compacted history plus the generated summary. Returns undefined if the
 * history cannot be compacted, the request is aborted, or summarization fails.
 */
export async function compactHistory(
  gateway: LlmGateway,
  model: ModelPath,
  history: readonly Message[],
  contextWindow: number | undefined,
  signal: AbortSignal,
  opts?: CompactHistoryOptions,
): Promise<CompactionResult | undefined> {
  const isManualIdle =
    opts?.mode === 'manual' &&
    history[history.length - 1]?.role === 'model' &&
    canCompactHistory(history);

  let toCompact: readonly Message[];
  let tail: readonly Message[];
  if (isManualIdle) {
    toCompact = history;
    tail = [];
  } else {
    const split = splitHistoryForCompaction(history);
    if (split === undefined) return undefined;
    toCompact = split.toCompact;
    tail = split.tail;
  }

  const transcript = serializeMessagesForCompaction(
    toCompact,
    transcriptCharBudget(contextWindow),
  );

  let summary = '';
  let failed = false;
  for await (const evt of gateway.createStream(
    model,
    {
      systemPrompt: COMPACTION_SYSTEM_PROMPT,
      messages: [
        {
          role: 'user',
          text: `Summarize the following conversation transcript:\n\n${transcript}`,
        },
      ],
      tools: [],
    },
    signal,
  )) {
    if (signal.aborted) return undefined;
    if (evt.type === 'text') {
      summary += evt.text;
    } else if (evt.type === 'stop' && evt.reason === 'error') {
      failed = true;
    }
  }

  if (signal.aborted || failed) return undefined;
  const trimmed = summary.trim();
  if (trimmed === '') return undefined;

  return {
    history: isManualIdle
      ? buildIdleCompactedHistory(trimmed)
      : buildCompactedHistory(trimmed, tail),
    summary: trimmed,
  };
}

function estimateMessageChars(msg: Message): number {
  switch (msg.role) {
    case 'user':
    case 'model':
      return msg.text.length;
    case 'tool-call':
      return msg.calls.reduce(
        (sum, c) => sum + c.name.length + safeJson(c.args).length,
        0,
      );
    case 'tool-result':
      return msg.results.reduce(
        (sum, r) => sum + r.name.length + r.result.length,
        0,
      );
  }
}

function sumChars(messages: readonly Message[]): number {
  let total = 0;
  for (const m of messages) {
    total += estimateMessageChars(m);
  }
  return total;
}

function findLastIndex<T>(
  arr: readonly T[],
  pred: (item: T) => boolean,
): number {
  for (let i = arr.length - 1; i >= 0; i--) {
    const item = arr[i];
    if (item !== undefined && pred(item)) return i;
  }
  return -1;
}

function truncateText(s: string, maxChars: number): string {
  if (s.length <= maxChars) return s;
  return `${s.slice(0, maxChars)}… (${s.length - maxChars} chars truncated)`;
}

function safeJson(value: unknown): string {
  try {
    return JSON.stringify(value);
  } catch {
    return String(value);
  }
}
