import {GoogleGenAI, mcpToTool} from '@google/genai';
import {Trace} from '../../public/trace';
import {Client} from '@modelcontextprotocol/sdk/client/index';

export function registerCommands(
  ai: GoogleGenAI,
  client: Client,
  trace: Trace,
) {
  trace.commands.registerCommand({
    id: `${trace.pluginId}#PerfettoMcpInfo`,
    name: 'MCP Query',
    callback: async () => {
      const result = await client.callTool({
        name: 'list_android_processes',
        arguments: {},
      });

      console.log('Perfetto MCP Info', result);
    },
  });

  trace.commands.registerCommand({
    id: `${trace.pluginId}#ListAndroidProcesses`,
    name: 'List Android Processes',
    callback: async () => {
      console.log('Listing Android Processes');
      try {
        const response = await ai.models.generateContent({
          model: 'gemini-2.0-flash',
          contents: `What android processes are in the trace?`,
          config: {
            tools: [mcpToTool(client)],
          },
        });
        console.log(response.text);
      } catch (error) {
        console.error('Error generating content:', error);
      }
    },
  });

  console.log('Perfetto MCP Commands registered');
}
