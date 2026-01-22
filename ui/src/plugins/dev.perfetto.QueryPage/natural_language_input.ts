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

import m from 'mithril';
import {z} from 'zod';
import {Button, ButtonVariant} from '../../widgets/button';
import {Intent} from '../../widgets/common';
import {Callout} from '../../widgets/callout';
import {Box} from '../../widgets/box';
import {Stack, StackAuto} from '../../widgets/stack';
import {HotkeyGlyphs} from '../../widgets/hotkey_glyphs';
import {TextInput} from '../../widgets/text_input';
import {Trace} from '../../public/trace';
import SqlModulesPlugin from '../dev.perfetto.SqlModules';
import {SqlTable} from '../dev.perfetto.SqlModules/sql_modules';
import {perfettoSqlTypeToString} from '../../trace_processor/perfetto_sql_type';
import {
  LanguageModelManager,
  LanguageModelProvider,
  LanguageModelProviderInfo,
} from '../../public/language_model';

// Re-export types for query_page.ts
export type LmProvider = string;
export type ProviderInfo = LanguageModelProviderInfo;

export const DEFAULT_SELECTED_TABLES = new Set([
  'slice',
  'gpu_slice',
  'thread',
  'process',
  'track',
  'thread_track',
  'process_track',
  'cpu_track',
  'gpu_track',
  'counter',
  'counter_track',
  'cpu_counter_track',
  'gpu_counter_track',
  'sched',
  'thread_state',
  'args',
  'instant',
  'flow',
]);

// SQL-specific settings storage for table selection
const SQL_LM_SETTINGS_KEY = 'sqlLmSettings';

const SQL_LM_SETTINGS_SCHEMA = z.object({
  selectedTables: z.array(z.string()).default([...DEFAULT_SELECTED_TABLES]),
});

class SqlLmSettingsStorage {
  private data: {selectedTables: string[]};

  constructor() {
    this.data = this.load();
  }

  getSelectedTables(): Set<string> {
    return new Set(this.data.selectedTables);
  }

  setSelectedTables(tables: Set<string>) {
    this.data.selectedTables = [...tables];
    this.save();
  }

  private load(): {selectedTables: string[]} {
    const defaultSettings = {selectedTables: [...DEFAULT_SELECTED_TABLES]};

    const value = window.localStorage.getItem(SQL_LM_SETTINGS_KEY);
    if (value === null) {
      return defaultSettings;
    }
    try {
      const res = SQL_LM_SETTINGS_SCHEMA.safeParse(JSON.parse(value));
      if (res.success) {
        return {selectedTables: res.data.selectedTables};
      }
      return defaultSettings;
    } catch {
      return defaultSettings;
    }
  }

  private save() {
    window.localStorage.setItem(SQL_LM_SETTINGS_KEY, JSON.stringify(this.data));
  }
}

const sqlLmSettingsStorage = new SqlLmSettingsStorage();

// Error thrown when model download is required
class GeminiNanoDownloadRequiredError extends Error {
  constructor() {
    super('Gemini Nano model needs to be downloaded');
    this.name = 'GeminiNanoDownloadRequiredError';
  }
}

// Settings storage for table selection
export const lmSettingsStorage = {
  getSelectedTables(): Set<string> {
    return sqlLmSettingsStorage.getSelectedTables();
  },
  setSelectedTables(tables: Set<string>) {
    sqlLmSettingsStorage.setSelectedTables(tables);
  },
};

// Helper to get a provider by ID
function getProvider(
  languageModels: LanguageModelManager,
  providerId: string,
): LanguageModelProvider | undefined {
  return languageModels.getProvider(providerId);
}

export interface NaturalLanguageInputAttrs {
  readonly trace: Trace;
  readonly onSqlGenerated: (sql: string) => void;
  readonly selectedTables: Set<string>;
  readonly onSelectedTablesChange: (tables: Set<string>) => void;
  readonly onClose?: () => void;
}

interface SchemaInfo {
  tables: TableInfo[];
}

interface TableInfo {
  name: string;
  description: string;
  columns: ColumnInfo[];
}

interface ColumnInfo {
  name: string;
  type: string;
  description?: string;
}

export class NaturalLanguageInput
  implements m.ClassComponent<NaturalLanguageInputAttrs>
{
  private naturalLanguageQuery = '';
  private isGenerating = false;
  private isDownloading = false;
  private downloadProgress: number | undefined = undefined;
  private error: string | undefined = undefined;
  private isSchemaContextExpanded = false;
  private availableTables: string[] = [];
  private isLoadingTables = false;
  private tableFilter = '';
  // Local override for the provider - doesn't modify global settings
  private selectedProviderOverride: LmProvider | undefined = undefined;

  async oninit({attrs}: m.CVnode<NaturalLanguageInputAttrs>): Promise<void> {
    await this.loadAvailableTables(attrs.trace);
  }

  private async loadAvailableTables(trace: Trace): Promise<void> {
    this.isLoadingTables = true;
    m.redraw();

    try {
      const sqlModulesPlugin = trace.plugins.getPlugin(SqlModulesPlugin);
      const sqlModules = sqlModulesPlugin.getSqlModules();

      const tableNames = new Set<string>();

      if (sqlModules) {
        const allTables = sqlModules.listTables();
        for (const table of allTables) {
          if (!table.name.startsWith('_')) {
            tableNames.add(table.name);
          }
        }
      }

      const result = await trace.engine.tryQuery(`
        SELECT name FROM sqlite_master
        WHERE type IN ('table', 'view')
        ORDER BY name
      `);

      if (result.ok) {
        for (
          const it = result.value.iter({name: 'str'});
          it.valid();
          it.next()
        ) {
          const tableName = it.name as string;
          if (!tableName.startsWith('_')) {
            tableNames.add(tableName);
          }
        }
      }

      const defaultTables = [...DEFAULT_SELECTED_TABLES];
      const nonDefaultTables = [...tableNames]
        .filter((name) => !DEFAULT_SELECTED_TABLES.has(name))
        .sort();
      this.availableTables = [...defaultTables.sort(), ...nonDefaultTables];
    } finally {
      this.isLoadingTables = false;
      m.redraw();
    }
  }

  view({attrs}: m.CVnode<NaturalLanguageInputAttrs>): m.Children {
    return m('.pf-nl-input', [
      m(Box, {className: 'pf-query-page__toolbar'}, [
        m(Stack, {orientation: 'horizontal'}, [
          m(Button, {
            label: 'Generate Query',
            icon: 'auto_awesome',
            loading: this.isGenerating || this.isDownloading,
            intent:
              this.isGenerating || this.isDownloading
                ? Intent.None
                : Intent.Primary,
            variant: ButtonVariant.Filled,
            disabled: !this.naturalLanguageQuery.trim(),
            onclick: () => this.handleGenerate(attrs),
          }),
          m(
            Stack,
            {
              orientation: 'horizontal',
              className: 'pf-query-page__hotkeys',
            },
            'or press',
            m(HotkeyGlyphs, {hotkey: 'Mod+Enter'}),
          ),
          m(StackAuto),
          this.renderProviderSelector(attrs),
          attrs.onClose &&
            m(Button, {
              icon: 'close',
              title: 'Close natural language query generator',
              onclick: () => {
                attrs.onClose?.();
              },
            }),
        ]),
      ]),
      m(
        '.pf-nl-input__textarea',
        {
          style: {
            padding: '8px',
            borderBottom: '1px solid var(--pf-color-border)',
          },
        },
        [
          m('textarea', {
            style: {
              width: '100%',
              minHeight: '60px',
              padding: '8px',
              borderRadius: '4px',
              border: '1px solid var(--pf-color-border)',
              backgroundColor: 'var(--pf-color-background)',
              resize: 'vertical',
              fontFamily: 'inherit',
              fontSize: '14px',
            },
            placeholder:
              'Describe what you want to query in natural language (e.g., "Show me the top 10 longest running slices with their thread names")',
            value: this.naturalLanguageQuery,
            oninput: (e: Event) => {
              this.naturalLanguageQuery = (
                e.target as HTMLTextAreaElement
              ).value;
            },
            onkeydown: (e: KeyboardEvent) => {
              if (e.key === 'Enter' && (e.metaKey || e.ctrlKey)) {
                e.preventDefault();
                this.handleGenerate(attrs);
              }
            },
          }),
        ],
      ),
      this.error &&
        m(Box, m(Callout, {icon: 'error', intent: Intent.Danger}, this.error)),
      this.isDownloading &&
        m(
          Box,
          m(Callout, {icon: 'download', intent: Intent.Primary}, [
            m(
              'div',
              {style: {marginBottom: '8px'}},
              `Downloading Gemini Nano model... ${Math.round(this.downloadProgress ?? 0)}%`,
            ),
            m(
              '.pf-nl-input__progress-bar',
              {
                style: {
                  width: '100%',
                  height: '8px',
                  backgroundColor: 'var(--pf-color-background-secondary)',
                  borderRadius: '4px',
                  overflow: 'hidden',
                },
              },
              [
                m('.pf-nl-input__progress-fill', {
                  style: {
                    width: `${this.downloadProgress ?? 0}%`,
                    height: '100%',
                    backgroundColor: 'var(--pf-color-primary)',
                    transition: 'width 0.3s ease-out',
                  },
                }),
              ],
            ),
          ]),
        ),
      this.renderSchemaContextSection(attrs),
    ]);
  }

  private renderSchemaContextSection(
    attrs: NaturalLanguageInputAttrs,
  ): m.Children {
    const selectedCount = attrs.selectedTables.size;

    return m(
      '.pf-nl-input__schema-context',
      {
        style: {
          borderTop: '1px solid var(--pf-color-border)',
        },
      },
      [
        m(
          '.pf-nl-input__schema-header',
          {
            style: {
              display: 'flex',
              alignItems: 'center',
              gap: '8px',
              padding: '8px 12px',
              cursor: 'pointer',
              userSelect: 'none',
            },
            onclick: () => {
              this.isSchemaContextExpanded = !this.isSchemaContextExpanded;
            },
          },
          [
            m(
              'span',
              {
                style: {
                  fontSize: '12px',
                  transition: 'transform 0.2s',
                  transform: this.isSchemaContextExpanded
                    ? 'rotate(90deg)'
                    : 'rotate(0deg)',
                },
              },
              '▶',
            ),
            m(
              'span',
              {style: {fontSize: '13px', fontWeight: '500'}},
              'Schema Context',
            ),
            m(
              'span',
              {
                style: {
                  fontSize: '11px',
                  color: 'var(--pf-color-text-muted)',
                },
              },
              `(${selectedCount} table${selectedCount !== 1 ? 's' : ''} selected)`,
            ),
          ],
        ),
        this.isSchemaContextExpanded &&
          m(
            '.pf-nl-input__schema-content',
            {
              style: {
                padding: '8px 12px',
                borderTop: '1px solid var(--pf-color-border)',
              },
            },
            [
              this.isLoadingTables
                ? m(
                    'span',
                    {style: {color: 'var(--pf-color-text-muted)'}},
                    'Loading tables...',
                  )
                : this.renderTableSelection(attrs),
            ],
          ),
      ],
    );
  }

  private renderTableSelection(attrs: NaturalLanguageInputAttrs): m.Children {
    const filteredTables = this.tableFilter
      ? this.availableTables.filter((name) =>
          name.toLowerCase().includes(this.tableFilter.toLowerCase()),
        )
      : this.availableTables;

    return m('.pf-nl-input__table-selection', [
      m(TextInput, {
        placeholder: 'Filter tables...',
        value: this.tableFilter,
        onInput: (value) => {
          this.tableFilter = value;
        },
      }),
      m(
        '.pf-nl-input__table-actions',
        {
          style: {
            display: 'flex',
            gap: '8px',
            marginTop: '8px',
          },
        },
        [
          m(Button, {
            label: 'Select All',
            onclick: () => {
              attrs.onSelectedTablesChange(new Set(this.availableTables));
            },
          }),
          m(Button, {
            label: 'Clear All',
            onclick: () => {
              attrs.onSelectedTablesChange(new Set());
            },
          }),
          m(Button, {
            label: 'Reset to Defaults',
            onclick: () => {
              attrs.onSelectedTablesChange(new Set(DEFAULT_SELECTED_TABLES));
            },
          }),
        ],
      ),
      m(
        '.pf-nl-input__table-list',
        {
          style: {
            maxHeight: '200px',
            overflowY: 'auto',
            border: '1px solid var(--pf-color-border)',
            borderRadius: '4px',
            marginTop: '8px',
          },
        },
        filteredTables.map((tableName) =>
          m(
            'label',
            {
              style: {
                display: 'flex',
                alignItems: 'center',
                gap: '8px',
                padding: '6px 8px',
                cursor: 'pointer',
                borderBottom: '1px solid var(--pf-color-border)',
                backgroundColor: DEFAULT_SELECTED_TABLES.has(tableName)
                  ? 'var(--pf-color-background-secondary)'
                  : 'transparent',
              },
            },
            [
              m('input', {
                type: 'checkbox',
                checked: attrs.selectedTables.has(tableName),
                onchange: () => {
                  const newSelected = new Set(attrs.selectedTables);
                  if (newSelected.has(tableName)) {
                    newSelected.delete(tableName);
                  } else {
                    newSelected.add(tableName);
                  }
                  attrs.onSelectedTablesChange(newSelected);
                },
              }),
              m(
                'span',
                {
                  style: {
                    fontSize: '12px',
                    fontFamily: 'monospace',
                  },
                },
                tableName,
              ),
              DEFAULT_SELECTED_TABLES.has(tableName) &&
                m(
                  'span',
                  {
                    style: {
                      fontSize: '10px',
                      color: 'var(--pf-color-text-muted)',
                      marginLeft: 'auto',
                    },
                  },
                  '(default)',
                ),
            ],
          ),
        ),
      ),
    ]);
  }

  private getSelectedProvider(
    languageModels: LanguageModelManager,
  ): LmProvider {
    // Use the local override if set, otherwise fall back to global preference
    if (this.selectedProviderOverride !== undefined) {
      return this.selectedProviderOverride;
    }
    return languageModels.getPreferredProviderId();
  }

  private renderProviderSelector(attrs: NaturalLanguageInputAttrs): m.Children {
    const providers = attrs.trace.languageModels
      .getProviders()
      .map((p) => p.info);
    const selectedProvider = this.getSelectedProvider(
      attrs.trace.languageModels,
    );
    return m(
      'select',
      {
        style: {
          padding: '4px 8px',
          borderRadius: '4px',
          border: '1px solid var(--pf-color-border)',
          backgroundColor: 'var(--pf-color-background)',
          cursor: 'pointer',
          fontSize: '12px',
        },
        value: selectedProvider,
        onchange: (e: Event) => {
          // Only update the local override, don't modify global settings
          this.selectedProviderOverride = (e.target as HTMLSelectElement)
            .value as LmProvider;
        },
      },
      providers.map((provider: ProviderInfo) =>
        m('option', {value: provider.id}, provider.name),
      ),
    );
  }

  private async handleGenerate(
    attrs: NaturalLanguageInputAttrs,
  ): Promise<void> {
    if (
      !this.naturalLanguageQuery.trim() ||
      this.isGenerating ||
      this.isDownloading
    ) {
      return;
    }

    this.isGenerating = true;
    this.error = undefined;
    m.redraw();

    try {
      const schema = await this.getSchemaInfo(attrs.trace);
      const sql = await this.generateSqlFromPrompt(
        attrs.trace.languageModels,
        this.naturalLanguageQuery,
        schema,
        this.getSelectedProvider(attrs.trace.languageModels),
        attrs.selectedTables,
        (partialSql: string) => {
          attrs.onSqlGenerated(partialSql);
          m.redraw();
        },
      );
      attrs.onSqlGenerated(sql);
    } catch (e) {
      if (e instanceof GeminiNanoDownloadRequiredError) {
        await this.handleDownloadAndRetry(attrs);
      } else {
        this.error = `Failed to generate SQL: ${e instanceof Error ? e.message : String(e)}`;
      }
    } finally {
      this.isGenerating = false;
      m.redraw();
    }
  }

  private async handleDownloadAndRetry(
    attrs: NaturalLanguageInputAttrs,
  ): Promise<void> {
    this.isGenerating = false;
    this.isDownloading = true;
    this.downloadProgress = 0;
    this.error = undefined;
    m.redraw();

    try {
      const provider = getProvider(attrs.trace.languageModels, 'gemini-nano');
      if (!provider || !provider.downloadModel) {
        throw new Error('Gemini Nano provider does not support downloading');
      }

      await provider.downloadModel((progress: number) => {
        this.downloadProgress = progress;
        m.redraw();
      });

      this.downloadProgress = 100;
      this.isDownloading = false;
      m.redraw();

      const schema = await this.getSchemaInfo(attrs.trace);
      this.isGenerating = true;
      m.redraw();

      const sql = await this.generateSqlFromPrompt(
        attrs.trace.languageModels,
        this.naturalLanguageQuery,
        schema,
        this.getSelectedProvider(attrs.trace.languageModels),
        attrs.selectedTables,
        (partialSql: string) => {
          attrs.onSqlGenerated(partialSql);
          m.redraw();
        },
      );
      attrs.onSqlGenerated(sql);
    } catch (e) {
      this.error = `Failed to download model: ${e instanceof Error ? e.message : String(e)}`;
    } finally {
      this.isDownloading = false;
      this.downloadProgress = undefined;
      this.isGenerating = false;
      m.redraw();
    }
  }

  private async generateSqlFromPrompt(
    languageModels: LanguageModelManager,
    prompt: string,
    schema: SchemaInfo,
    providerId: LmProvider,
    selectedTables: Set<string>,
    onProgress?: (partialSql: string) => void,
  ): Promise<string> {
    const systemPrompt = this.buildSystemPrompt(schema, selectedTables);

    const progressCallback = onProgress
      ? (partialResponse: string) =>
          onProgress(this.extractSql(partialResponse))
      : undefined;

    const provider = getProvider(languageModels, providerId);
    if (!provider) {
      throw new Error(`Unknown provider: ${providerId}`);
    }

    const status = await provider.checkStatus();
    if (status.status === 'not-supported') {
      throw new Error(status.message);
    }
    if (status.status === 'unavailable') {
      throw new Error(status.message);
    }
    if (status.status === 'downloadable' || status.status === 'downloading') {
      throw new GeminiNanoDownloadRequiredError();
    }

    try {
      const response = await provider.generate({
        systemPrompt,
        userPrompt: prompt,
        onProgress: progressCallback,
      });
      return this.extractSql(response);
    } catch (e) {
      const errorMessage = e instanceof Error ? e.message : String(e);
      if (errorMessage.includes('The input is too large')) {
        throw new Error(
          `${errorMessage} Try reducing the number of tables for the Schema Context.`,
        );
      }
      throw e;
    }
  }

  private buildSystemPrompt(
    schema: SchemaInfo,
    selectedTables: Set<string>,
  ): string {
    const relevantTables = this.selectRelevantTables(
      schema.tables,
      selectedTables,
    );

    let schemaDescription = `You are an expert SQL query generator for Perfetto trace analysis.
Your task is to generate valid PerfettoSQL queries based on user requests.

PerfettoSQL is based on SQLite with some extensions. Key things to know:
- Use single quotes for string literals, not double quotes
- Use LIMIT to restrict results when appropriate
- Common tables include: slice, thread, process, track, and counter

Available tables and their schemas:

`;

    for (const table of relevantTables) {
      schemaDescription += `\n## Table: ${table.name}\n`;
      if (table.description) {
        schemaDescription += `Description: ${table.description}\n`;
      }
      schemaDescription += 'Columns:\n';
      for (const col of table.columns) {
        schemaDescription += `  - ${col.name} (${col.type})`;
        if (col.description) {
          schemaDescription += `: ${col.description}`;
        }
        schemaDescription += '\n';
      }
    }

    schemaDescription += `

Important guidelines:
1. Only output the SQL query, nothing else
2. Do not include any explanation or markdown formatting
3. Use proper JOINs when relating tables
4. Always use LIMIT unless the user specifically asks for all results
5. Use meaningful column aliases when appropriate
6. For duration/timestamp columns, values are typically in nanoseconds

Common query patterns:
- To get slice with thread info: SELECT s.*, t.name as thread_name FROM slice s JOIN thread_track tt ON s.track_id = tt.id JOIN thread t ON tt.utid = t.utid
- To get process info: SELECT * FROM process WHERE name LIKE '%pattern%'
- To analyze CPU scheduling: SELECT * FROM sched WHERE utid > 0
`;

    return schemaDescription;
  }

  private selectRelevantTables(
    tables: TableInfo[],
    selectedTableNames: Set<string>,
  ): TableInfo[] {
    const selected: TableInfo[] = [];

    for (const table of tables) {
      if (selectedTableNames.has(table.name)) {
        selected.push(table);
      }
    }

    return selected;
  }

  private extractSql(response: string): string {
    const sqlBlockMatch = response.match(/```sql\n?([\s\S]*?)\n?```/i);
    if (sqlBlockMatch) {
      return sqlBlockMatch[1].trim();
    }

    const codeBlockMatch = response.match(/```\n?([\s\S]*?)\n?```/);
    if (codeBlockMatch) {
      return codeBlockMatch[1].trim();
    }

    const lines = response.trim().split('\n');
    const sqlLines: string[] = [];
    let inSql = false;

    for (const line of lines) {
      const upperLine = line.trim().toUpperCase();
      if (
        upperLine.startsWith('SELECT') ||
        upperLine.startsWith('WITH') ||
        upperLine.startsWith('INSERT') ||
        upperLine.startsWith('UPDATE') ||
        upperLine.startsWith('DELETE') ||
        upperLine.startsWith('CREATE') ||
        upperLine.startsWith('DROP') ||
        upperLine.startsWith('INCLUDE PERFETTO')
      ) {
        inSql = true;
      }

      if (inSql) {
        sqlLines.push(line);
        if (line.trim().endsWith(';')) {
          break;
        }
      }
    }

    if (sqlLines.length > 0) {
      return sqlLines.join('\n').trim();
    }

    return response.trim();
  }

  private async getSchemaInfo(trace: Trace): Promise<SchemaInfo> {
    const sqlModulesPlugin = trace.plugins.getPlugin(SqlModulesPlugin);
    const sqlModules = sqlModulesPlugin.getSqlModules();

    const tables: TableInfo[] = [];

    if (sqlModules) {
      const allTables = sqlModules.listTables();
      for (const table of allTables) {
        tables.push(this.convertTableToInfo(table));
      }
    }

    const coreTables = await this.getCoreTables(trace);
    for (const table of coreTables) {
      if (!tables.find((t) => t.name === table.name)) {
        tables.push(table);
      }
    }

    return {tables};
  }

  private convertTableToInfo(table: SqlTable): TableInfo {
    return {
      name: table.name,
      description: table.description,
      columns: table.columns.map((col) => ({
        name: col.name,
        type: col.type ? perfettoSqlTypeToString(col.type) : 'unknown',
        description: col.description,
      })),
    };
  }

  private async getCoreTables(trace: Trace): Promise<TableInfo[]> {
    const tables: TableInfo[] = [];

    const result = await trace.engine.tryQuery(`
      SELECT name FROM sqlite_master
      WHERE type IN ('table', 'view')
      ORDER BY name
    `);

    if (result.ok) {
      for (const it = result.value.iter({name: 'str'}); it.valid(); it.next()) {
        const tableName = it.name as string;
        const columnsResult = await trace.engine.tryQuery(
          `PRAGMA table_info('${tableName}')`,
        );

        const columns: ColumnInfo[] = [];
        if (columnsResult.ok) {
          for (
            const colIt = columnsResult.value.iter({name: 'str', type: 'str'});
            colIt.valid();
            colIt.next()
          ) {
            columns.push({
              name: colIt.name as string,
              type: colIt.type as string,
            });
          }
        }

        tables.push({
          name: tableName,
          description: '',
          columns,
        });
      }
    }

    return tables;
  }
}
