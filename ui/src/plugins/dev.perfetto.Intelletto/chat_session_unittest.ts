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
): {
  session: ChatSession;
  requests: Request[];
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
  );
  return {session, requests};
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
