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
import {ensureDefined} from '../../base/assert';
import type {Setting} from '../../public/settings';
import {
  PROVIDERS_SETTING_SCHEMA,
  type ProvidersSetting,
} from '../dev.perfetto.Llm/config';
import {LlmGateway, type ModelPath} from '../dev.perfetto.Llm/gateway';
import type {
  Protocol,
  Request,
  StreamEvent,
} from '../dev.perfetto.Llm/protocol';
import {ChatSession} from './chat_session';
import {ContextRegistry} from './context';
import {ConversationStore} from './conversation_store';
import {ToolRegistry} from './tools';

const MODEL: ModelPath = {providerId: 'test-provider', modelId: 'test-model'};

function createFakeProvidersSetting(): Setting<ProvidersSetting> {
  let value: ProvidersSetting = [];
  return {
    id: 'test#Providers',
    name: 'Providers',
    description: 'Fake providers setting for unit tests',
    schema: PROVIDERS_SETTING_SCHEMA,
    defaultValue: [],
    get isDefault() {
      return value.length === 0;
    },
    get: () => value,
    set: (v) => {
      value = v;
    },
    reset: () => {
      value = [];
    },
    [Symbol.dispose]: () => {},
  };
}

function setupSession(
  streamFn: (
    req: Request,
    signal?: AbortSignal,
  ) => AsyncGenerator<StreamEvent, void, void>,
  tools = new ToolRegistry(),
  opts?: {
    readonly contextWindow?: number;
    readonly maxCompactionTokens?: number | (() => number);
  },
): {
  session: ChatSession;
  requests: Request[];
  gateway: LlmGateway;
  store: ConversationStore;
} {
  const requests: Request[] = [];
  const protocol: Protocol = {
    id: 'test-proto',
    label: 'Test',
    capabilities: {nativeToolCalling: true, streaming: true},
    credentialFields: [],
    async *createStream(_model, request, _credentials, signal) {
      requests.push({...request, messages: [...request.messages]});
      yield* streamFn(request, signal);
    },
  };

  const gateway = new LlmGateway(createFakeProvidersSetting());
  gateway.registerProtocol(protocol);
  gateway.registerProvider({
    id: MODEL.providerId,
    protocol: protocol.id,
    credentials: {},
    models: [
      {
        id: MODEL.modelId,
        modelName: MODEL.modelId,
        roles: ['agentic'],
        contextWindow: opts?.contextWindow,
      },
    ],
  });

  const store = new ConversationStore();
  const session = new ChatSession(
    gateway,
    tools,
    new ContextRegistry(),
    'You are a test assistant.',
    store,
    {model: MODEL},
    opts?.maxCompactionTokens,
  );
  return {session, requests, gateway, store};
}

describe('ChatSession retry', () => {
  test('allows retrying after backend error stop event without duplicating user turn', async () => {
    let attempt = 0;
    const {session, requests} = setupSession(async function* () {
      attempt++;
      if (attempt === 1) {
        yield {
          type: 'stop',
          reason: 'error',
          error: {kind: 'network', message: '503 Service Unavailable'},
        };
        return;
      }
      yield {type: 'text', text: 'Recovered answer.'};
      yield {type: 'stop', reason: 'end'};
    });

    await session.send('Why is rendering slow?', MODEL);

    expect(session.canRetry).toBe(true);
    expect(
      session.lines.some(
        (l) => l.role === 'error' && l.text === '503 Service Unavailable',
      ),
    ).toBe(true);

    await session.retry(MODEL);

    expect(attempt).toBe(2);
    expect(session.canRetry).toBe(false);
    // The stale error line was removed and the user prompt was not duplicated.
    expect(session.lines.filter((l) => l.role === 'error')).toHaveLength(0);
    expect(session.lines.filter((l) => l.role === 'user')).toHaveLength(1);
    expect(
      session.lines.some(
        (l) => l.role === 'ai' && l.text === 'Recovered answer.',
      ),
    ).toBe(true);
    // Second request carries exactly one user message (no duplicate).
    expect(ensureDefined(requests[1]).messages).toEqual([
      {role: 'user', text: 'Why is rendering slow?'},
    ]);
  });

  test('allows retrying when stream throws an exception', async () => {
    let attempt = 0;
    const {session} = setupSession(async function* () {
      attempt++;
      if (attempt === 1) {
        throw new Error('connection reset');
      }
      yield {type: 'text', text: 'OK'};
      yield {type: 'stop', reason: 'end'};
    });

    await session.send('Hello', MODEL);

    expect(session.canRetry).toBe(true);
    expect(session.lines.some((l) => l.role === 'error')).toBe(true);

    await session.retry(MODEL);

    expect(attempt).toBe(2);
    expect(session.canRetry).toBe(false);
    expect(session.lines.filter((l) => l.role === 'error')).toHaveLength(0);
    expect(session.lines.some((l) => l.role === 'ai' && l.text === 'OK')).toBe(
      true,
    );
  });

  test('retries from tool-result when failure occurs on a follow-up round', async () => {
    const tools = new ToolRegistry();
    tools.registerTool({
      name: 'echo',
      description: 'Echo text',
      shape: {msg: z.string()},
      callback: async ({msg}) => `echo:${msg}`,
    });

    let callNum = 0;
    const {session, requests} = setupSession(async function* () {
      callNum++;
      if (callNum === 1) {
        yield {
          type: 'tool-call',
          call: {id: 'c1', name: 'echo', args: {msg: 'hi'}},
        };
        yield {type: 'stop', reason: 'tool-calls'};
        return;
      }
      if (callNum === 2) {
        yield {
          type: 'stop',
          reason: 'error',
          error: {kind: 'rate-limit', message: '429 Too Many Requests'},
        };
        return;
      }
      yield {type: 'text', text: 'Final answer after tool.'};
      yield {type: 'stop', reason: 'end'};
    }, tools);

    await session.send('Run echo', MODEL);

    expect(session.canRetry).toBe(true);

    await session.retry(MODEL);

    expect(callNum).toBe(3);
    expect(session.canRetry).toBe(false);
    // Tool call ran only once; retry resumed from the saved tool-result.
    expect(ensureDefined(requests[2]).messages).toEqual([
      {role: 'user', text: 'Run echo'},
      {
        role: 'tool-call',
        calls: [{id: 'c1', name: 'echo', args: {msg: 'hi'}}],
      },
      {
        role: 'tool-result',
        results: [{id: 'c1', name: 'echo', result: 'echo:hi', isError: false}],
      },
    ]);
    expect(
      session.lines.some(
        (l) => l.role === 'ai' && l.text === 'Final answer after tool.',
      ),
    ).toBe(true);
  });

  test('does not allow retrying after user cancellation or normal completion', async () => {
    let resolveTurn: (() => void) | undefined;
    const {session} = setupSession(async function* () {
      await new Promise<void>((resolve) => {
        resolveTurn = resolve;
      });
      yield {type: 'text', text: 'Done'};
      yield {type: 'stop', reason: 'end'};
    });

    const turnPromise = session.send('First', MODEL);
    session.cancel();
    resolveTurn?.();
    await turnPromise;

    expect(session.canRetry).toBe(false);

    // Normal completion also leaves canRetry false.
    const secondPromise = session.send('Second', MODEL);
    resolveTurn?.();
    await secondPromise;
    expect(session.canRetry).toBe(false);
  });
});

describe('ChatSession dynamic registries', () => {
  test('resolves tools, context, and system prompt dynamically across trace switches', async () => {
    const toolsA = new ToolRegistry();
    toolsA.registerTool({
      name: 'tool_a',
      description: 'Tool for trace A',
      shape: {},
      callback: async () => 'from_a',
    });
    const contextA = new ContextRegistry();
    contextA.registerContextProvider({
      id: 'trace',
      description: 'Trace A context format',
      getContext: () => ({summary: 'Trace A', data: 'Current trace: A'}),
    });

    const toolsB = new ToolRegistry();
    toolsB.registerTool({
      name: 'tool_b',
      description: 'Tool for trace B',
      shape: {},
      callback: async () => 'from_b',
    });
    const contextB = new ContextRegistry();
    contextB.registerContextProvider({
      id: 'trace',
      description: 'Trace B context format',
      getContext: () => ({summary: 'Trace B', data: 'Current trace: B'}),
    });

    let activeTools = toolsA;
    let activeContext = contextA;

    const requests: Request[] = [];
    const protocol: Protocol = {
      id: 'test-proto',
      label: 'Test',
      capabilities: {nativeToolCalling: true, streaming: true},
      credentialFields: [],
      async *createStream(_model, request) {
        requests.push({...request, messages: [...request.messages]});
        yield {type: 'text', text: 'ok'};
        yield {type: 'stop', reason: 'end'};
      },
    };
    const gateway = new LlmGateway(createFakeProvidersSetting());
    gateway.registerProtocol(protocol);
    gateway.registerProvider({
      id: MODEL.providerId,
      protocol: protocol.id,
      credentials: {},
      models: [
        {id: MODEL.modelId, modelName: MODEL.modelId, roles: ['agentic']},
      ],
    });

    const session = new ChatSession(
      gateway,
      () => activeTools,
      () => activeContext,
      () => activeContext.descriptions().join('\n'),
      new ConversationStore(),
      {model: MODEL},
    );

    await session.send('First question', MODEL);

    // Switch to trace B mid-session.
    activeTools = toolsB;
    activeContext = contextB;

    await session.send('Second question', MODEL);

    expect(requests).toHaveLength(2);
    expect(ensureDefined(requests[0]).tools?.map((t) => t.name)).toEqual([
      'tool_a',
    ]);
    expect(ensureDefined(requests[0]).systemPrompt).toContain(
      'Trace A context format',
    );
    expect(ensureDefined(requests[1]).tools?.map((t) => t.name)).toEqual([
      'tool_b',
    ]);
    expect(ensureDefined(requests[1]).systemPrompt).toContain(
      'Trace B context format',
    );
    expect(ensureDefined(requests[1]).messages[2]).toEqual({
      role: 'user',
      text: 'Context for this question:\n- Trace B: Current trace: B\n\nQuestion: Second question',
    });
  });
});

describe('ChatSession auto-compaction', () => {
  test('proactively compacts older history once usage reaches 80% of contextWindow', async () => {
    let call = 0;
    const {session, requests} = setupSession(
      async function* (req) {
        call++;
        if (call === 1) {
          yield {type: 'text', text: 'First detailed answer.'};
          // 85% of the 10_000-token context window.
          yield {type: 'usage', usage: {totalTokens: 8_500}};
          yield {type: 'stop', reason: 'end'};
          return;
        }
        if (call === 2) {
          // Compaction summarization request has no tools.
          expect(req.tools).toEqual([]);
          yield {
            type: 'text',
            text: 'Discussed first question and found root cause X.',
          };
          yield {type: 'usage', usage: {totalTokens: 9_000}};
          yield {type: 'stop', reason: 'end'};
          return;
        }
        yield {type: 'text', text: 'Second answer using summary.'};
        yield {type: 'usage', usage: {totalTokens: 1_200}};
        yield {type: 'stop', reason: 'end'};
      },
      new ToolRegistry(),
      {contextWindow: 10_000},
    );

    await session.send('First question', MODEL);
    await session.send('Second question', MODEL);

    expect(call).toBe(3);
    // The third request carries the compacted summary + ack + second question.
    const postCompactMsgs = ensureDefined(requests[2]).messages;
    expect(postCompactMsgs).toHaveLength(3);
    expect(postCompactMsgs[0]?.role).toBe('user');
    if (postCompactMsgs[0]?.role === 'user') {
      expect(postCompactMsgs[0].text).toContain(
        'Discussed first question and found root cause X.',
      );
    }
    expect(postCompactMsgs[1]?.role).toBe('model');
    expect(postCompactMsgs[2]).toEqual({role: 'user', text: 'Second question'});

    // Visible transcript keeps earlier messages AND records a compaction marker.
    expect(
      session.lines.some(
        (l) => l.role === 'user' && l.text === 'First question',
      ),
    ).toBe(true);
    expect(
      session.lines.some(
        (l) =>
          l.role === 'compaction' &&
          l.text === 'Discussed first question and found root cause X.',
      ),
    ).toBe(true);
    expect(session.totalTokens).toBe(1_200);
    expect(session.compacting).toBe(false);
  });

  test('compacts reactively and retries on context-length error when contextWindow is unset', async () => {
    let call = 0;
    const {session, requests} = setupSession(async function* (req) {
      call++;
      if (call === 1) {
        yield {type: 'text', text: 'Initial response.'};
        yield {type: 'stop', reason: 'end'};
        return;
      }
      if (call === 2) {
        // Turn 2 overflows context length on the backend.
        yield {
          type: 'stop',
          reason: 'error',
          error: {
            kind: 'context-length',
            message: 'maximum context length exceeded',
          },
        };
        return;
      }
      if (call === 3) {
        // Reactive compaction call.
        expect(req.tools).toEqual([]);
        yield {type: 'text', text: 'Compacted summary of turn 1.'};
        yield {type: 'stop', reason: 'end'};
        return;
      }
      // Automatic retry of turn 2 succeeds with compacted history.
      yield {type: 'text', text: 'Recovered after compaction.'};
      yield {type: 'usage', usage: {totalTokens: 900}};
      yield {type: 'stop', reason: 'end'};
    });

    await session.send('Turn 1', MODEL);
    await session.send('Turn 2', MODEL);

    expect(call).toBe(4);
    expect(session.lines.filter((l) => l.role === 'error')).toHaveLength(0);
    expect(
      session.lines.some(
        (l) =>
          l.role === 'compaction' && l.text === 'Compacted summary of turn 1.',
      ),
    ).toBe(true);
    expect(
      session.lines.some(
        (l) => l.role === 'ai' && l.text === 'Recovered after compaction.',
      ),
    ).toBe(true);
    expect(ensureDefined(requests[3]).messages[2]).toEqual({
      role: 'user',
      text: 'Turn 2',
    });
  });

  test('falls back to sending uncompacted turn if proactive compaction fails', async () => {
    let call = 0;
    const {session, requests} = setupSession(
      async function* () {
        call++;
        if (call === 1) {
          yield {type: 'text', text: 'Answer 1'};
          yield {type: 'usage', usage: {totalTokens: 9_000}};
          yield {type: 'stop', reason: 'end'};
          return;
        }
        if (call === 2) {
          // Proactive compaction fails transiently.
          yield {
            type: 'stop',
            reason: 'error',
            error: {kind: 'network', message: 'temporary failure'},
          };
          return;
        }
        yield {type: 'text', text: 'Answer 2 anyway'};
        yield {type: 'stop', reason: 'end'};
      },
      new ToolRegistry(),
      {contextWindow: 10_000},
    );

    await session.send('Q1', MODEL);
    await session.send('Q2', MODEL);

    expect(call).toBe(3);
    expect(session.lines.filter((l) => l.role === 'compaction')).toHaveLength(
      0,
    );
    expect(session.lines.filter((l) => l.role === 'error')).toHaveLength(0);
    expect(
      session.lines.some(
        (l) => l.role === 'ai' && l.text === 'Answer 2 anyway',
      ),
    ).toBe(true);
    // Original history was preserved when compaction failed.
    expect(ensureDefined(requests[2]).messages).toEqual([
      {role: 'user', text: 'Q1'},
      {role: 'model', text: 'Answer 1'},
      {role: 'user', text: 'Q2'},
    ]);
  });

  test('proactively compacts restored sessions whose stored totalTokens already exceed threshold', async () => {
    let call = 0;
    const {gateway, store, requests} = setupSession(
      async function* () {
        call++;
        if (call === 1) {
          yield {type: 'text', text: 'Restored summary.'};
          yield {type: 'stop', reason: 'end'};
          return;
        }
        yield {type: 'text', text: 'Answer after restore.'};
        yield {type: 'usage', usage: {totalTokens: 1_500}};
        yield {type: 'stop', reason: 'end'};
      },
      new ToolRegistry(),
      {contextWindow: 10_000},
    );

    const restored = new ChatSession(
      gateway,
      new ToolRegistry(),
      new ContextRegistry(),
      'System',
      store,
      {
        stored: {
          id: 'saved-1',
          title: 'Old conversation',
          createdAt: 1000,
          updatedAt: 2000,
          model: MODEL,
          lines: [
            {role: 'user', text: 'Old question'},
            {role: 'ai', text: 'Old answer'},
          ],
          history: [
            {role: 'user', text: 'Old question'},
            {role: 'model', text: 'Old answer'},
          ],
          totalTokens: 8_500,
        },
      },
    );

    await restored.send('Follow-up after reload', MODEL);

    expect(call).toBe(2);
    expect(
      restored.lines.some(
        (l) => l.role === 'compaction' && l.text === 'Restored summary.',
      ),
    ).toBe(true);
    expect(ensureDefined(requests[1]).messages[2]).toEqual({
      role: 'user',
      text: 'Follow-up after reload',
    });
  });

  test('compacts mid-turn during a multi-round tool loop', async () => {
    const tools = new ToolRegistry();
    tools.registerTool({
      name: 'big_query',
      description: 'Returns large data',
      shape: {step: z.number()},
      callback: async ({step}) => `step_${step}: ${'x'.repeat(2_000)}`,
    });

    let call = 0;
    const {session, requests} = setupSession(
      async function* (req) {
        call++;
        if (call === 1) {
          yield {
            type: 'tool-call',
            call: {id: 'c1', name: 'big_query', args: {step: 1}},
          };
          yield {type: 'usage', usage: {totalTokens: 4_000}};
          yield {type: 'stop', reason: 'tool-calls'};
          return;
        }
        if (call === 2) {
          yield {
            type: 'tool-call',
            call: {id: 'c2', name: 'big_query', args: {step: 2}},
          };
          // Crosses 80% of 10_000 -> next round triggers mid-turn compaction.
          yield {type: 'usage', usage: {totalTokens: 8_200}};
          yield {type: 'stop', reason: 'tool-calls'};
          return;
        }
        if (call === 3) {
          expect(req.tools).toEqual([]);
          yield {type: 'text', text: 'Step 1 completed with large data.'};
          yield {type: 'stop', reason: 'end'};
          return;
        }
        yield {type: 'text', text: 'Final answer after mid-turn compaction.'};
        yield {type: 'usage', usage: {totalTokens: 2_000}};
        yield {type: 'stop', reason: 'end'};
      },
      tools,
      {contextWindow: 10_000},
    );

    await session.send('Analyze everything', MODEL);

    expect(call).toBe(4);
    // Request 4 starts with summary user message followed by the kept tool-call + tool-result pair.
    const msgs = ensureDefined(requests[3]).messages;
    expect(msgs.map((m) => m.role)).toEqual([
      'user',
      'tool-call',
      'tool-result',
    ]);
    expect(
      session.lines.some(
        (l) =>
          l.role === 'ai' &&
          l.text === 'Final answer after mid-turn compaction.',
      ),
    ).toBe(true);
  });

  test('supports manual compaction of an idle conversation and disables repeat compaction', async () => {
    let call = 0;
    const {session, requests} = setupSession(async function* (req) {
      call++;
      if (call === 1) {
        yield {type: 'text', text: 'First answer.'};
        yield {type: 'usage', usage: {totalTokens: 2_500}};
        yield {type: 'stop', reason: 'end'};
        return;
      }
      if (call === 2) {
        expect(req.tools).toEqual([]);
        yield {type: 'text', text: 'Manual summary of first turn.'};
        yield {type: 'stop', reason: 'end'};
        return;
      }
      yield {type: 'text', text: 'Answer after manual compact.'};
      yield {type: 'usage', usage: {totalTokens: 600}};
      yield {type: 'stop', reason: 'end'};
    });

    expect(session.canCompact).toBe(false);
    await session.send('First question', MODEL);
    expect(session.canCompact).toBe(true);

    await session.compact(MODEL);

    expect(call).toBe(2);
    expect(session.canCompact).toBe(false);
    expect(session.totalTokens).toBeUndefined();
    expect(
      session.lines.some(
        (l) =>
          l.role === 'compaction' && l.text === 'Manual summary of first turn.',
      ),
    ).toBe(true);

    // Repeating compact while nothing new was added is a no-op.
    await session.compact(MODEL);
    expect(call).toBe(2);

    // Next turn sends [summary, ack, user message] cleanly.
    await session.send('Follow-up question', MODEL);
    expect(call).toBe(3);
    const msgs = ensureDefined(requests[2]).messages;
    expect(msgs.map((m) => m.role)).toEqual(['user', 'model', 'user']);
    expect(msgs[2]).toEqual({role: 'user', text: 'Follow-up question'});
    expect(session.canCompact).toBe(true);
  });

  test('surfaces an error without losing history when manual compaction fails', async () => {
    let call = 0;
    const {session, requests} = setupSession(async function* () {
      call++;
      if (call === 1) {
        yield {type: 'text', text: 'First answer.'};
        yield {type: 'stop', reason: 'end'};
        return;
      }
      if (call === 2) {
        yield {
          type: 'stop',
          reason: 'error',
          error: {kind: 'network', message: 'offline'},
        };
        return;
      }
      yield {type: 'text', text: 'Second answer.'};
      yield {type: 'stop', reason: 'end'};
    });

    await session.send('First question', MODEL);
    await session.compact(MODEL);

    expect(
      session.lines.some(
        (l) =>
          l.role === 'error' && l.text === 'Failed to compact conversation.',
      ),
    ).toBe(true);
    expect(session.canCompact).toBe(true);

    // Original history is still intact for subsequent turns.
    await session.send('Second question', MODEL);
    expect(ensureDefined(requests[2]).messages).toEqual([
      {role: 'user', text: 'First question'},
      {role: 'model', text: 'First answer.'},
      {role: 'user', text: 'Second question'},
    ]);
  });

  test('triggers proactive compaction at the configured maxCompactionTokens cap on large context windows', async () => {
    let call = 0;
    const {session, requests} = setupSession(
      async function* () {
        call++;
        if (call === 1) {
          yield {type: 'text', text: 'First answer.'};
          // Only 16% of 1M context window, but above the 150k cap.
          yield {type: 'usage', usage: {totalTokens: 160_000}};
          yield {type: 'stop', reason: 'end'};
          return;
        }
        if (call === 2) {
          yield {type: 'text', text: 'Capped compaction summary.'};
          yield {type: 'stop', reason: 'end'};
          return;
        }
        yield {type: 'text', text: 'Second answer.'};
        yield {type: 'usage', usage: {totalTokens: 2_000}};
        yield {type: 'stop', reason: 'end'};
      },
      new ToolRegistry(),
      {contextWindow: 1_000_000, maxCompactionTokens: 150_000},
    );

    await session.send('First question', MODEL);
    await session.send('Second question', MODEL);

    expect(call).toBe(3);
    expect(
      session.lines.some(
        (l) =>
          l.role === 'compaction' && l.text === 'Capped compaction summary.',
      ),
    ).toBe(true);
    expect(ensureDefined(requests[2]).messages[2]).toEqual({
      role: 'user',
      text: 'Second question',
    });
  });
});
