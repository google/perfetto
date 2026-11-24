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
import type {Setting} from '../../public/settings';
import type {ProvidersSetting} from '../dev.perfetto.Llm/config';
import {LlmGateway, type ModelPath} from '../dev.perfetto.Llm/gateway';
import type {Protocol, Request} from '../dev.perfetto.Llm/protocol';
import {ChatSession} from './chat_session';
import {ContextRegistry} from './context';
import {ToolRegistry} from './tools';

const MODEL: ModelPath = {providerId: 'test-provider', modelId: 'test-model'};

function createFakeProvidersSetting(): Setting<ProvidersSetting> {
  return {
    id: 'test.providers',
    name: 'Providers',
    description: '',
    schema: z.array(z.never()),
    defaultValue: [],
    isDefault: true,
    get: () => [],
    set: () => {},
    reset: () => {},
    [Symbol.dispose]: () => {},
  };
}

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
    );

    expect(session.systemPrompt).toContain('Trace A context format');
    session.input = 'First question';
    await session.send(MODEL);

    // Switch to trace B mid-session.
    activeTools = toolsB;
    activeContext = contextB;

    expect(session.systemPrompt).toContain('Trace B context format');
    session.input = 'Second question';
    await session.send(MODEL);

    expect(requests).toHaveLength(2);
    expect(requests[0]!.tools?.map((t) => t.name)).toEqual(['tool_a']);
    expect(requests[0]!.systemPrompt).toContain('Trace A context format');
    expect(requests[1]!.tools?.map((t) => t.name)).toEqual(['tool_b']);
    expect(requests[1]!.systemPrompt).toContain('Trace B context format');
    expect(requests[1]!.messages[2]).toEqual({
      role: 'user',
      text: 'Context for this question:\n- Trace B: Current trace: B\n\nQuestion: Second question',
    });
  });
});
