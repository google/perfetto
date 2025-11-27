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

import {Trace} from '../../public/trace';
import {App} from '../../public/app';
import {PerfettoPlugin} from '../../public/plugin';
import {McpServer} from '@modelcontextprotocol/sdk/server/mcp.js';
import {Client} from '@modelcontextprotocol/sdk/client/index.js';
import {InMemoryTransport} from '@modelcontextprotocol/sdk/inMemory.js';
import {
  CallableTool,
  FunctionCallingConfigMode,
  GoogleGenAI,
  mcpToTool,
} from '@google/genai';
import {registerTraceTools} from './tracetools';
import {z} from 'zod';
import {Setting} from 'src/public/settings';
import {ChatPage} from './chat_page';
import m from 'mithril';
import {registerUiTools} from './uitools';

export default class PerfettoMcpPlugin implements PerfettoPlugin {
  static readonly id = 'com.google.PerfettoMcp';
  static readonly description = `
    This plugin adds support for a AI Chat window. 
    This is backed by Gemini and implement MCP (Model Context Protocol).
    While Gemini can understand and generate SQL queries, the tools allow Gemini to interact with the trace data directly
    to answer your queries.
    `;

  static tokenSetting: Setting<string>;
  static promptSetting: Setting<string>;
  static thoughtsSetting: Setting<boolean>;
  static showTokensSetting: Setting<boolean>;
  static modelNameSetting: Setting<string>;

  static onActivate(app: App): void {
    PerfettoMcpPlugin.tokenSetting = app.settings.register({
      id: `${PerfettoMcpPlugin.id}#TokenSetting`,
      name: 'Gemini Token',
      description: 'Gemini API Token.',
      schema: z.string(),
      defaultValue: '',
      requiresReload: true,
    });

    PerfettoMcpPlugin.thoughtsSetting = app.settings.register({
      id: `${PerfettoMcpPlugin.id}#ThoughtsSetting`,
      name: 'Show Thoughts and Tool Calls',
      description: 'Show thoughts and tool calls in the chat.',
      schema: z.boolean(),
      defaultValue: true,
    });

    PerfettoMcpPlugin.showTokensSetting = app.settings.register({
      id: `${PerfettoMcpPlugin.id}#ShowTokensSetting`,
      name: 'Show Token Usage',
      description: 'Show detailed token usage.',
      schema: z.boolean(),
      defaultValue: true,
    });

    PerfettoMcpPlugin.modelNameSetting = app.settings.register({
      id: `${PerfettoMcpPlugin.id}#ModelNameSetting`,
      name: 'Gemini Model',
      description: 'The Gemini model to use, such as gemini-2.5-pro.',
      schema: z.string(),
      defaultValue: 'gemini-2.5-pro',
      requiresReload: true,
    });

    PerfettoMcpPlugin.promptSetting = app.settings.register({
      id: `${PerfettoMcpPlugin.id}#PromptSetting`,
      name: 'Gemini Prompt',
      description:
        'Upload a .txt or .md file containing the initial Gemini prompt.',
      schema: z.string(),
      defaultValue: '',
      requiresReload: true,
      render: (setting) => {
        const handleFileSelect = (event: {target: HTMLInputElement}) => {
          const file = event.target.files?.[0];

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

        return m('input', {
          type: 'file',
          accept: ['.txt', '.md'],
          onchange: handleFileSelect,
        });
      },
    });
  }

  async onTraceLoad(trace: Trace): Promise<void> {
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

    const tool: CallableTool = mcpToTool(client);

    const ai = new GoogleGenAI({apiKey: PerfettoMcpPlugin.tokenSetting.get()});

    const chat = await ai.chats.create({
      model: PerfettoMcpPlugin.modelNameSetting.get(),
      config: {
        systemInstruction:
          'You are an expert in analyzing perfetto traces. \n\n' +
          PerfettoMcpPlugin.promptSetting.get(),
        tools: [tool],
        toolConfig: {
          functionCallingConfig: {
            mode: FunctionCallingConfigMode.AUTO,
          },
        },
        thinkingConfig: {
          includeThoughts: true,
          thinkingBudget: -1, // Automatic
        },
        automaticFunctionCalling: {
          maximumRemoteCalls: 20,
        },
      },
    });

    trace.pages.registerPage({
      route: '/aichat',
      render: () => {
        return m(ChatPage, {
          trace,
          chat,
          showThoughts: PerfettoMcpPlugin.thoughtsSetting,
          showTokens: PerfettoMcpPlugin.showTokensSetting,
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
}
