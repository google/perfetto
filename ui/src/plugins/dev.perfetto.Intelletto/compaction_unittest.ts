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

import {ensureDefined} from '../../base/assert';
import type {Message} from '../dev.perfetto.Llm/protocol';
import {
  buildCompactedHistory,
  buildIdleCompactedHistory,
  canCompactHistory,
  compactionTriggerTokens,
  isCompactedSummary,
  serializeMessagesForCompaction,
  splitHistoryForCompaction,
  transcriptCharBudget,
} from './compaction';

describe('splitHistoryForCompaction', () => {
  test('returns undefined when history has 0 or 1 message', () => {
    expect(splitHistoryForCompaction([])).toBeUndefined();
    expect(
      splitHistoryForCompaction([{role: 'user', text: 'Only question'}]),
    ).toBeUndefined();
  });

  test('keeps latest user message verbatim at start of a fresh turn', () => {
    const history: Message[] = [
      {role: 'user', text: 'First question'},
      {role: 'model', text: 'First answer'},
      {role: 'user', text: 'Second question'},
    ];
    const split = ensureDefined(splitHistoryForCompaction(history));
    expect(split.toCompact).toEqual([
      {role: 'user', text: 'First question'},
      {role: 'model', text: 'First answer'},
    ]);
    expect(split.tail).toEqual([{role: 'user', text: 'Second question'}]);
  });

  test('splits mid-turn before the latest tool-call without separating tool-call and tool-result', () => {
    const history: Message[] = [
      {role: 'user', text: 'Investigate startup'},
      {
        role: 'tool-call',
        calls: [{id: 'c1', name: 'run_query', args: {sql: 'SELECT 1'}}],
      },
      {
        role: 'tool-result',
        results: [
          {
            id: 'c1',
            name: 'run_query',
            result: 'x'.repeat(2_000),
            isError: false,
          },
        ],
      },
      {
        role: 'tool-call',
        calls: [{id: 'c2', name: 'run_query', args: {sql: 'SELECT 2'}}],
      },
      {
        role: 'tool-result',
        results: [{id: 'c2', name: 'run_query', result: 'ok', isError: false}],
      },
    ];
    const split = ensureDefined(splitHistoryForCompaction(history));
    expect(split.toCompact).toEqual(history.slice(0, 3));
    expect(split.tail).toEqual(history.slice(3));
    expect(split.tail[0]?.role).toBe('tool-call');
  });

  test('falls back to compacting everything when a single tool round dominates history', () => {
    const history: Message[] = [
      {role: 'user', text: 'Run query'},
      {
        role: 'tool-call',
        calls: [{id: 'c1', name: 'run_query', args: {sql: 'SELECT *'}}],
      },
      {
        role: 'tool-result',
        results: [
          {
            id: 'c1',
            name: 'run_query',
            result: 'x'.repeat(10_000),
            isError: false,
          },
        ],
      },
    ];
    const split = ensureDefined(splitHistoryForCompaction(history));
    expect(split.toCompact).toEqual(history);
    expect(split.tail).toEqual([]);
  });
});

describe('buildCompactedHistory', () => {
  test('inserts model acknowledgment when tail starts with user message', () => {
    const compacted = buildCompactedHistory('Key findings here.', [
      {role: 'user', text: 'Next question'},
    ]);
    expect(compacted).toHaveLength(3);
    expect(compacted[0]?.role).toBe('user');
    expect(compacted[1]?.role).toBe('model');
    expect(compacted[2]).toEqual({role: 'user', text: 'Next question'});
  });

  test('omits extra model acknowledgment when tail starts with tool-call', () => {
    const tail: Message[] = [
      {
        role: 'tool-call',
        calls: [{id: 'c1', name: 'run_query', args: {sql: 'SELECT 1'}}],
      },
      {
        role: 'tool-result',
        results: [{id: 'c1', name: 'run_query', result: '1', isError: false}],
      },
    ];
    const compacted = buildCompactedHistory('Summary.', tail);
    expect(compacted).toHaveLength(3);
    expect(compacted[0]?.role).toBe('user');
    expect(compacted[1]).toEqual(tail[0]);
    expect(compacted[2]).toEqual(tail[1]);
  });

  test('appends continuation prompt when tail is empty', () => {
    const compacted = buildCompactedHistory('Summary.', []);
    expect(compacted).toHaveLength(1);
    expect(compacted[0]?.role).toBe('user');
    if (compacted[0]?.role === 'user') {
      expect(compacted[0].text).toContain('Summary.');
      expect(compacted[0].text).toContain('continue');
    }
  });

  test('buildIdleCompactedHistory produces a summary/ack pair recognized by isCompactedSummary', () => {
    const idle = buildIdleCompactedHistory('Completed summary.');
    expect(idle).toHaveLength(2);
    expect(idle[0]?.role).toBe('user');
    expect(idle[1]?.role).toBe('model');
    expect(isCompactedSummary(idle)).toBe(true);
    expect(canCompactHistory(idle)).toBe(false);
    expect(
      canCompactHistory([...idle, {role: 'user', text: 'Next turn'}]),
    ).toBe(true);
  });
});

describe('serializeMessagesForCompaction', () => {
  test('formats all message roles and respects maxChars budget', () => {
    const messages: Message[] = [
      {role: 'user', text: 'Why is frame 42 slow?'},
      {
        role: 'tool-call',
        calls: [{name: 'run_query', args: {sql: 'SELECT 1'}}],
      },
      {
        role: 'tool-result',
        results: [{name: 'run_query', result: 'Evidence id: Q1\n[{"a":1}]'}],
      },
      {role: 'model', text: 'Because of layout inflation [[Q1]].'},
    ];
    const full = serializeMessagesForCompaction(messages, 10_000);
    expect(full).toContain('User:\nWhy is frame 42 slow?');
    expect(full).toContain('Tool call run_query({"sql":"SELECT 1"})');
    expect(full).toContain('Tool result run_query:\nEvidence id: Q1');
    expect(full).toContain('Assistant:\nBecause of layout inflation [[Q1]].');

    const bounded = serializeMessagesForCompaction(messages, 120);
    expect(bounded.length).toBeLessThanOrEqual(120);
    expect(bounded).toContain('omitted for length');
  });

  test('computes transcriptCharBudget from contextWindow or fallback with upper cap', () => {
    expect(transcriptCharBudget(undefined)).toBe(120_000);
    expect(transcriptCharBudget(0)).toBe(120_000);
    expect(transcriptCharBudget(100_000)).toBe(200_000);
    expect(transcriptCharBudget(1_000_000)).toBe(400_000);
    expect(transcriptCharBudget(500)).toBe(4_000);
  });

  test('compactionTriggerTokens takes the smaller of 80% contextWindow and cap', () => {
    // 1M context window capped at 150k.
    expect(compactionTriggerTokens(1_000_000, 150_000)).toBe(150_000);
    // 32k context window triggers at 80% (25.6k) when cap is higher.
    expect(compactionTriggerTokens(32_000, 150_000)).toBe(25_600);
    // Cap = 0 disables absolute cap and uses 80% of context window.
    expect(compactionTriggerTokens(1_000_000, 0)).toBe(800_000);
    // Unknown context window still uses positive cap.
    expect(compactionTriggerTokens(undefined, 150_000)).toBe(150_000);
    // Neither configured -> undefined.
    expect(compactionTriggerTokens(undefined, 0)).toBeUndefined();
    expect(compactionTriggerTokens(undefined, undefined)).toBeUndefined();
  });
});
