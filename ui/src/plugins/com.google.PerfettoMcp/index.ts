// Copyright (C) 2025 The Android Open Source Project
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

import { Trace } from '../../public/trace';
import { App } from '../../public/app';
import { MetricVisualisation } from '../../public/plugin';
import { PerfettoPlugin } from '../../public/plugin';
import { McpServer } from '@modelcontextprotocol/sdk/server/mcp.js';
import { Client } from '@modelcontextprotocol/sdk/client/index.js';
import { InMemoryTransport } from '@modelcontextprotocol/sdk/inMemory.js';
import { CallableTool, FunctionCallingConfigMode, GoogleGenAI, mcpToTool } from '@google/genai';
import { registerTraceTools } from './tracetools';
import { z } from 'zod';
import { Setting } from 'src/public/settings';
import { registerCommands } from './commands';
import { ChatPage } from './chat_page';
import m from 'mithril';
import { registerUiTools } from './uitools';
export default class PerfettoMcpPlugin implements PerfettoPlugin {
  static readonly id = 'com.google.PerfettoMcp';

  static tokenSetting: Setting<string>;
  static promptSetting: Setting<string>;


  static onActivate(app: App): void {
    PerfettoMcpPlugin.tokenSetting = app.settings.register({
      id: `${app.pluginId}#TokenSetting`,
      name: 'Gemini Token',
      description: 'Gemini API Token.',
      schema: z.string(),
      defaultValue: '',
    });

    PerfettoMcpPlugin.promptSetting = app.settings.register({
      id: `${app.pluginId}#PromptSetting`,
      name: 'Gemini Prompt',
      description: 'Upload a .txt file containing the initial Gemini prompt. (minimum of 2048 tokens)',
      schema: z.string(),
      defaultValue: "",
      render: (setting) => {
        const handleFileSelect = (event: any) => {
          const input = event.target as HTMLInputElement;
          const file = input.files?.[0];

          if (!file) {
            return;
          }

          const reader = new FileReader();

          reader.onload = (e: ProgressEvent<FileReader>) => {
            const fileContent = e.target?.result;
            if (typeof fileContent === 'string') {
              setting.set(fileContent);
            }
          };

          reader.onerror = () => {
            console.error('FileReader error:', reader.error);
          };

          reader.readAsText(file);
        };

        return m(
          'div', {
          style: 'padding: 10px; border: 1px solid #ccc; border-radius: 8px;'
        },
          [
            m('input', {
              type: 'file',
              accept: ['.txt', '.md'],
              style: 'margin-top: 10px; display: block;',
              onchange: handleFileSelect,
            }),
            m('p', {
              style: 'margin-top: 8px; font-style: italic; font-size: 0.9em; color: #555;'
            }, 'Select a file')
          ]
        );
      },
    });
  }

  async onTraceLoad(trace: Trace): Promise<void> {
    console.log('PerfettoMcpPlugin onTraceLoad');
    const mcpServer = new McpServer({
      name: 'PerfettoMcp',
      version: '1.0.0',
    });

    registerTraceTools(mcpServer, trace.engine);
    registerUiTools(mcpServer, trace);

    const client = new Client({
      name: 'PerfettoMcpClient',
      version: '1.0',
    });

    const [clientTransport, serverTransport] =
      InMemoryTransport.createLinkedPair();

    await Promise.all([
      client.connect(clientTransport),
      mcpServer.server.connect(serverTransport),
    ]);

    var tool: CallableTool = mcpToTool(client)

    const ai = new GoogleGenAI({ apiKey: PerfettoMcpPlugin.tokenSetting.get() });

    var model = 'gemini-2.5-pro';

    var chat = await ai.chats.create({
      model: model,
      config: {
        systemInstruction: "You are an expert in analyzing perfetto traces. \n\n" + PerfettoMcpPlugin.promptSetting.get(),
        tools: [tool],
        toolConfig: {
          functionCallingConfig: {
            mode: FunctionCallingConfigMode.AUTO,
          },
        },
        thinkingConfig: {
          includeThoughts: true,
          thinkingBudget: -1,
        },
        automaticFunctionCalling: {
          maximumRemoteCalls: 100,
        }
      }
    })

    registerCommands(ai, client, trace);

    trace.pages.registerPage({
      route: '/aichat',
      render: () => {
        return m(ChatPage, {
          trace,
          chat
        });
      },
    });
    trace.sidebar.addMenuItem({
      section: 'current_trace',
      text: 'AI Chat',
      href: '#!/aichat',
      icon: 'smart_toy',
      sortOrder: 10,
    });
  }

  static metricVisualisations(): MetricVisualisation[] {
    return [];
  }
}
