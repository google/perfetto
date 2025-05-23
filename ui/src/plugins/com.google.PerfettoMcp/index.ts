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
import { GoogleGenAI } from '@google/genai';
import { registerTraceTools } from './tracetools';
import { z } from 'zod';
import { Setting } from 'src/public/settings';
import { registerCommands } from './commands';
import { ChatPage } from './chat_page';
import m from 'mithril';
export default class PerfettoMcpPlugin implements PerfettoPlugin {
  static readonly id = 'com.google.PerfettoMcp';

  static tokenSetting: Setting<string>;


  static onActivate(app: App): void {
    PerfettoMcpPlugin.tokenSetting = app.settings.register({
      id: `${app.pluginId}#TokenSetting`,
      name: 'Gemini Token',
      description: 'Gemini API Token.',
      schema: z.string(),
      defaultValue: '',
    });
  }

  async onTraceLoad(trace: Trace): Promise<void> {
    console.log('PerfettoMcpPlugin onTraceLoad');
    const mcpServer = new McpServer({
      name: 'PerfettoMcp',
      version: '1.0.0',
    });

    registerTraceTools(mcpServer, trace.engine);

    console.log('Server started!');

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

    const ai = new GoogleGenAI({ apiKey: PerfettoMcpPlugin.tokenSetting.get() });

    registerCommands(ai, client, trace);

    trace.pages.registerPage({
      route: '/aichat',
      render: () => {
        return m(ChatPage, {
          trace,
          ai,
          client
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
