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

import m from 'mithril';
import type {LlmGateway} from '../../../../ui/src/plugins/dev.perfetto.Llm/gateway';

const LOCAL_PROVIDER_ID = 'local-openai-loopback';
const DEFAULT_LOCAL_ENDPOINT = 'http://localhost:8080/v1';

let probed = false;

// Silently probes an OpenAI-compatible local server on http://localhost:8080/v1
// (already allowlisted by Perfetto's CSP — works with llama-server, Ollama,
// vLLM, LiteLLM, or any local OpenAI-compatible proxy). If reachable, registers
// its advertised models as a zero-config runtime provider on LlmPlugin.gateway.
export async function probeLocalOpenAiProvider(
  gateway: LlmGateway,
  endpoint = DEFAULT_LOCAL_ENDPOINT,
): Promise<boolean> {
  if (probed) return false;
  probed = true;
  if (gateway.listProviders().some((p) => p.id === LOCAL_PROVIDER_ID)) {
    return true;
  }

  const ac = new AbortController();
  const timer = setTimeout(() => ac.abort(), 1500);
  try {
    const resp = await fetch(`${endpoint}/models`, {
      method: 'GET',
      signal: ac.signal,
    });
    if (!resp.ok) return false;
    const json = (await resp.json()) as {
      readonly data?: readonly {readonly id?: string}[];
    };
    const ids = (json.data ?? [])
      .map((x) => x.id ?? '')
      .filter((id) => id !== '');
    if (ids.length === 0) return false;

    gateway.registerProvider({
      id: LOCAL_PROVIDER_ID,
      label: 'Local LLM (localhost:8080)',
      protocol: 'openai-compatible',
      credentials: {endpoint, apiKey: ''},
      models: ids.map((modelName, idx) => ({
        id: `local-${idx}-${modelName.replace(/[^a-zA-Z0-9._-]/g, '_')}`,
        label: modelName,
        modelName,
        roles: ['agentic', 'flash'],
      })),
    });
    m.redraw();
    return true;
  } catch {
    return false;
  } finally {
    clearTimeout(timer);
  }
}
