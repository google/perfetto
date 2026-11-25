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

import {PRODUCT_TOOLS} from '../../../../ui/src/gen/products_sql';
import type {ToolDef} from '../../../../ui/src/plugins/dev.perfetto.Llm/protocol';
import type {Engine} from '../../../../ui/src/trace_processor/engine';
import {jsonSchemaToZod} from './json_schema_to_zod';
import type {
  AgentContextSnapshot,
  EvidenceStore,
  HandoffAction,
  InvestigationNote,
  ProductAgentAdapter,
  ProductToolManifest,
  ProductToolSpec,
} from './types';

const MAX_SQL_ROWS = 200;

function sqlLiteral(val: unknown): string {
  if (val === null || val === undefined) return 'NULL';
  if (typeof val === 'number') {
    return Number.isFinite(val) ? String(val) : 'NULL';
  }
  if (typeof val === 'bigint') return String(val);
  if (typeof val === 'boolean') return val ? '1' : '0';
  const s = String(val).replace(/'/g, "''");
  return `'${s}'`;
}

export function bindSqlParams(
  sql: string,
  params: Record<string, unknown>,
): string {
  return sql.replace(/\$([a-zA-Z_][a-zA-Z0-9_]*)/g, (_m, name: string) => {
    if (Object.prototype.hasOwnProperty.call(params, name)) {
      return sqlLiteral(params[name]);
    }
    return 'NULL';
  });
}

export async function runSqlQuery(
  engine: Engine,
  sql: string,
  maxRows = 50,
): Promise<{
  readonly columns: ReadonlyArray<string>;
  readonly rows: ReadonlyArray<Record<string, unknown>>;
  readonly truncated: boolean;
}> {
  const res = await engine.query(sql);
  const cols = res.columns();
  const limit = Math.max(1, Math.min(MAX_SQL_ROWS, Math.floor(maxRows)));
  const rows: Record<string, unknown>[] = [];
  let truncated = false;
  const it = res.iter({});
  for (; it.valid(); it.next()) {
    if (rows.length >= limit) {
      truncated = true;
      break;
    }
    const row: Record<string, unknown> = {};
    for (const c of cols) {
      const v = it.get(c);
      if (typeof v === 'bigint') {
        row[c] =
          v >= BigInt(Number.MIN_SAFE_INTEGER) &&
          v <= BigInt(Number.MAX_SAFE_INTEGER)
            ? Number(v)
            : v.toString();
      } else if (v instanceof Uint8Array) {
        row[c] = `<${v.byteLength} bytes>`;
      } else {
        row[c] = v;
      }
    }
    rows.push(row);
  }
  return {columns: cols, rows, truncated};
}

export interface ToolRunResult {
  readonly output: unknown;
  readonly evidenceId?: string;
  readonly handoff?: HandoffAction;
  readonly isError?: boolean;
}

export type ApprovalRequester = (prompt: {
  readonly title: string;
  readonly command: string;
}) => Promise<boolean>;

export class DeclarativeToolRunner {
  private readonly commonTools: ReadonlyArray<ProductToolSpec>;
  private readonly productTools: ReadonlyArray<ProductToolSpec>;
  private readonly uiTools: ReadonlyArray<ProductToolSpec>;

  constructor(private readonly adapter: ProductAgentAdapter) {
    const commonManifest = PRODUCT_TOOLS['common/common_tools.json'] as
      | ProductToolManifest
      | undefined;
    const productManifest = PRODUCT_TOOLS[adapter.manifestKey] as
      | ProductToolManifest
      | undefined;
    this.commonTools = commonManifest?.tools ?? [];
    this.productTools = productManifest?.tools ?? [];
    this.uiTools = this.buildUiTools();
  }

  private buildUiTools(): ProductToolSpec[] {
    if (!this.adapter.ui) return [];
    const targets = this.adapter.ui.navTargets;
    return [
      {
        name: 'ui_navigate',
        kind: 'ui',
        modes: ['live', 'snapshot', 'trace', 'compare'],
        description: `Navigate the user's UI to a specific screen or explainer tab so they see the data you are discussing. Valid targets: ${targets.join(', ')}.`,
        parameters: {
          type: 'object',
          properties: {
            target: {
              type: 'string',
              enum: targets,
              description: 'Target tab/screen to open.',
            },
            subtab: {
              type: 'string',
              description: 'Optional process name or sub-explainer key.',
            },
          },
          required: ['target'],
        },
      },
      {
        name: 'suggest_handoff',
        kind: 'ui',
        modes: ['live', 'snapshot', 'trace', 'compare'],
        description:
          'Surface an interactive handoff card when the investigation reaches this product boundary and a specialist product (e.g. memscope for single-app heap/smaps) should take over.',
        parameters: {
          type: 'object',
          properties: {
            product: {
              type: 'string',
              enum: ['memscope'],
              description: 'Specialist product to hand off to.',
            },
            process: {
              type: 'string',
              description: 'Optional suspect process name.',
            },
            reason: {
              type: 'string',
              description: 'One-sentence reason why this crosses into the specialist tool.',
            },
          },
          required: ['product', 'reason'],
        },
      },
    ];
  }

  listSpecs(ctx?: AgentContextSnapshot): ReadonlyArray<ProductToolSpec> {
    const mode = ctx?.mode;
    const all = [...this.productTools, ...this.commonTools, ...this.uiTools];
    return all.filter((t) => {
      if (t.kind === 'device' && !this.adapter.device) return false;
      if (mode && mode !== 'none' && !t.modes.includes(mode)) {
        // Allow compare mode to still use snapshot SQL tools against primary snapshot.
        if (!(mode === 'compare' && t.modes.includes('snapshot'))) {
          return false;
        }
      }
      return true;
    });
  }

  listLlmToolDefs(ctx?: AgentContextSnapshot): ToolDef[] {
    return this.listSpecs(ctx).map((spec) => ({
      name: spec.name,
      description: spec.description,
      inputSchema: jsonSchemaToZod(spec.parameters),
    }));
  }

  async execute(
    name: string,
    args: Record<string, unknown>,
    ctx: AgentContextSnapshot,
    evidence: EvidenceStore,
    notes: InvestigationNote[],
    requestApproval?: ApprovalRequester,
  ): Promise<ToolRunResult> {
    const spec = this.listSpecs(ctx).find((t) => t.name === name);
    if (!spec) {
      return {
        output: {error: `Tool "${name}" is not available in mode "${ctx.mode}".`},
        isError: true,
      };
    }

    const baseParams: Record<string, unknown> = {
      start_ts: ctx.window?.startTs ?? null,
      end_ts: ctx.window?.endTs ?? null,
      process: ctx.focusProcess ?? null,
      ...args,
    };

    try {
      if (spec.kind === 'sql' && spec.sql) {
        if (!ctx.engine) {
          return {
            output: {error: 'No TraceProcessor engine active yet. Connect a device or open a trace first.'},
            isError: true,
          };
        }
        const bound = bindSqlParams(spec.sql, baseParams);
        const maxRows =
          typeof args.limit === 'number'
            ? args.limit
            : (spec.default_limit ?? 50);
        const res = await runSqlQuery(ctx.engine, bound, maxRows);
        const rec = evidence.record(name, args, ctx.label, res, bound);
        return {
          evidenceId: rec.id,
          output: {
            evidence_id: rec.id,
            scope: ctx.label,
            row_count: res.rows.length,
            truncated: res.truncated,
            rows: res.rows,
          },
        };
      }

      if (spec.kind === 'sql_multi' && spec.queries) {
        if (!ctx.engine) {
          return {
            output: {error: 'No TraceProcessor engine active yet.'},
            isError: true,
          };
        }
        const sections: Record<string, unknown> = {};
        const sqlLog: string[] = [];
        const maxRows = spec.default_limit ?? 25;
        for (const [secName, rawSql] of Object.entries(spec.queries)) {
          const bound = bindSqlParams(rawSql, baseParams);
          sqlLog.push(`-- ${secName}\n${bound}`);
          try {
            const res = await runSqlQuery(ctx.engine, bound, maxRows);
            sections[secName] = res.rows;
          } catch (e) {
            sections[secName] = {error: String(e)};
          }
        }
        const rec = evidence.record(
          name,
          args,
          ctx.label,
          sections,
          sqlLog.join('\n\n'),
        );
        return {
          evidenceId: rec.id,
          output: {
            evidence_id: rec.id,
            scope: ctx.label,
            sections,
          },
        };
      }

      if (spec.kind === 'sql_adhoc') {
        if (!ctx.engine) {
          return {
            output: {error: 'No TraceProcessor engine active yet.'},
            isError: true,
          };
        }
        const sql = String(args.sql ?? '').trim();
        if (!sql) return {output: {error: 'Missing sql argument'}, isError: true};
        const maxRows =
          typeof args.max_rows === 'number' ? args.max_rows : 50;
        const res = await runSqlQuery(ctx.engine, sql, maxRows);
        const rec = evidence.record(name, args, ctx.label, res, sql);
        return {
          evidenceId: rec.id,
          output: {
            evidence_id: rec.id,
            scope: ctx.label,
            columns: res.columns,
            row_count: res.rows.length,
            truncated: res.truncated,
            rows: res.rows,
          },
        };
      }

      if (spec.kind === 'note') {
        const title = String(args.title ?? '').trim();
        const summary = String(args.summary ?? '').trim();
        const ids = Array.isArray(args.evidence_ids)
          ? args.evidence_ids.map(String)
          : [];
        const missing = ids.filter((id) => !evidence.get(id));
        const verified = ids.length > 0 && missing.length === 0;
        const entry: InvestigationNote = {
          title,
          summary,
          evidenceIds: ids,
          missingEvidenceIds: missing,
          verified,
          atMs: Date.now(),
        };
        notes.push(entry);
        return {
          output: {
            recorded: true,
            verified,
            missing_evidence_ids: missing,
            note_count: notes.length,
          },
        };
      }

      if (spec.kind === 'device') {
        if (!this.adapter.device) {
          return {
            output: {error: 'No live device connected.'},
            isError: true,
          };
        }
        if (name === 'device_status') {
          const st = await this.adapter.device.getStatus();
          const rec = evidence.record(name, args, ctx.label, st);
          return {evidenceId: rec.id, output: {evidence_id: rec.id, ...((st as object) ?? {})}};
        }
        if (name === 'take_snapshot') {
          const snap = await this.adapter.device.takeSnapshot(
            typeof args.label === 'string' ? args.label : undefined,
          );
          const rec = evidence.record(name, args, ctx.label, snap);
          return {evidenceId: rec.id, output: {evidence_id: rec.id, ...((snap as object) ?? {})}};
        }
        if (name === 'run_workload') {
          const workload = String(args.workload ?? '');
          const command =
            typeof args.command === 'string' ? args.command : undefined;
          if (workload === 'custom') {
            if (!command) {
              return {output: {error: 'Missing command for custom workload.'}, isError: true};
            }
            if (requestApproval) {
              const ok = await requestApproval({
                title: 'Approve custom device command',
                command,
              });
              if (!ok) {
                return {
                  output: {cancelled: true, reason: 'User declined custom shell command.'},
                  isError: true,
                };
              }
            }
          }
          const out = await this.adapter.device.runWorkload({
            workload,
            command,
            durationS:
              typeof args.duration_s === 'number' ? args.duration_s : undefined,
            snapshotAfter: Boolean(args.snapshot_after),
          });
          const rec = evidence.record(name, args, ctx.label, out);
          return {evidenceId: rec.id, output: {evidence_id: rec.id, ...((out as object) ?? {})}};
        }
      }

      if (spec.kind === 'ui' && this.adapter.ui) {
        if (name === 'ui_navigate') {
          const msg = this.adapter.ui.navigate(
            String(args.target ?? ''),
            typeof args.subtab === 'string' ? args.subtab : undefined,
          );
          return {output: {navigated: true, message: msg}};
        }
        if (name === 'suggest_handoff') {
          const handoff = this.adapter.ui.buildHandoff({
            product: String(args.product ?? 'memscope'),
            process: typeof args.process === 'string' ? args.process : undefined,
            reason: String(args.reason ?? ''),
          });
          if (!handoff) {
            return {output: {error: 'Handoff target unavailable'}, isError: true};
          }
          return {
            handoff,
            output: {
              suggested: true,
              product: handoff.product,
              url: handoff.url,
            },
          };
        }
      }

      return {output: {error: `Unhandled tool "${name}"`}, isError: true};
    } catch (e) {
      return {
        output: {error: e instanceof Error ? e.message : String(e)},
        isError: true,
      };
    }
  }
}
