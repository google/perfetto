import {GoogleGenAI, mcpToTool} from '@google/genai';
import {Trace} from '../../public/trace';
import {Client} from '@modelcontextprotocol/sdk/client/index';

export function registerCommands(
  ai: GoogleGenAI,
  client: Client,
  trace: Trace,
  modelName: string,
) {
  trace.commands.registerCommand({
    id: `${trace.pluginId}#PerfettoMcpInfo`,
    name: 'MCP Query',
    callback: async () => {
      const result = await client.callTool({
        name: 'list_android_processes',
        arguments: {},
      });

      console.log('Processes in Trace', result);
    },
  });

  trace.commands.registerCommand({
    id: `${trace.pluginId}#ListAndroidProcesses`,
    name: 'List Android Processes',
    callback: async () => {
      const response = await ai.models.generateContent({
        model: modelName,
        contents: `What android processes are in the trace?`,
        config: {
          tools: [mcpToTool(client)],
        },
      });

      console.log('Processes in Trace', response);
    },
  });
}
