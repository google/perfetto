#!/usr/bin/env node
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

// Generic stdio MCP server & CLI tool runner shared across Perfetto products.
// Reads the exact same declarative `products/<id>/tools/tools.json`,
// `products/common/tools/common_tools.json`, and `products/<id>/sql/<pkg>/*.sql`
// used by the browser Agentic Sidebar, guaranteeing a 1:1 tool contract.
//
// Usage:
//   node products/common/mcp/server.mjs --product sysmon --trace /path/to.pftrace
//   node products/common/mcp/server.mjs --product sysmon --trace /path/to.pftrace --call sysmon_pulse
//   node products/common/mcp/server.mjs --product memscope --trace /path/to.pftrace --list-tools

import {execFileSync, spawnSync} from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import readline from 'node:readline';
import {fileURLToPath} from 'node:url';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const ROOT_DIR = path.resolve(__dirname, '../../..');

function parseArgs(argv) {
  const out = {
    product: 'sysmon',
    trace: undefined,
    tp: undefined,
    call: undefined,
    callArgs: '{}',
    listTools: false,
    adb: false,
  };
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (a === '--product') out.product = argv[++i];
    else if (a === '--trace') out.trace = argv[++i];
    else if (a === '--tp') out.tp = argv[++i];
    else if (a === '--call') out.call = argv[++i];
    else if (a === '--args') out.callArgs = argv[++i];
    else if (a === '--list-tools') out.listTools = true;
    else if (a === '--adb') out.adb = true;
  }
  return out;
}

function findTraceProcessor(override) {
  if (override && fs.existsSync(override)) return override;
  const candidates = [
    path.join(ROOT_DIR, 'out/linux_clang_release/trace_processor_shell'),
    path.join(ROOT_DIR, 'out/linux_debug/trace_processor_shell'),
    path.join(ROOT_DIR, 'tools/trace_processor'),
  ];
  for (const c of candidates) {
    if (fs.existsSync(c)) return c;
  }
  return 'trace_processor_shell';
}

function sqlLiteral(val) {
  if (val === null || val === undefined) return 'NULL';
  if (typeof val === 'number') return Number.isFinite(val) ? String(val) : 'NULL';
  if (typeof val === 'boolean') return val ? '1' : '0';
  return `'${String(val).replace(/'/g, "''")}'`;
}

function bindSqlParams(sql, params) {
  return sql.replace(/\$([a-zA-Z_][a-zA-Z0-9_]*)/g, (_m, name) => {
    if (Object.prototype.hasOwnProperty.call(params, name)) {
      return sqlLiteral(params[name]);
    }
    return 'NULL';
  });
}

function parseCsvRows(csvText, maxRows = 100) {
  const lines = [];
  let cur = '';
  let inQuotes = false;
  for (let i = 0; i < csvText.length; i++) {
    const ch = csvText[i];
    if (ch === '"') {
      if (inQuotes && csvText[i + 1] === '"') {
        cur += '"';
        i++;
      } else {
        inQuotes = !inQuotes;
      }
      cur += ch;
    } else if ((ch === '\n' || ch === '\r') && !inQuotes) {
      if (cur.length > 0) lines.push(cur);
      cur = '';
    } else {
      cur += ch;
    }
  }
  if (cur.length > 0) lines.push(cur);
  if (lines.length === 0) return {columns: [], rows: [], truncated: false};

  const splitLine = (line) => {
    const cells = [];
    let val = '';
    let q = false;
    for (let i = 0; i < line.length; i++) {
      const ch = line[i];
      if (ch === '"') {
        if (q && line[i + 1] === '"') {
          val += '"';
          i++;
        } else {
          q = !q;
        }
      } else if (ch === ',' && !q) {
        cells.push(val);
        val = '';
      } else {
        val += ch;
      }
    }
    cells.push(val);
    return cells;
  };

  const columns = splitLine(lines[0]);
  const rows = [];
  let truncated = false;
  for (let r = 1; r < lines.length; r++) {
    if (rows.length >= maxRows) {
      truncated = true;
      break;
    }
    const vals = splitLine(lines[r]);
    if (vals.length !== columns.length) continue;
    const obj = {};
    for (let c = 0; c < columns.length; c++) {
      const raw = vals[c];
      if (raw === '[NULL]') obj[columns[c]] = null;
      else if (/^-?\d+(\.\d+)?$/.test(raw)) obj[columns[c]] = Number(raw);
      else obj[columns[c]] = raw;
    }
    rows.push(obj);
  }
  return {columns, rows, truncated};
}

class ProductMcpRuntime {
  constructor(opts) {
    this.opts = opts;
    this.tpBin = findTraceProcessor(opts.tp);
    this.productDir = path.join(ROOT_DIR, 'products', opts.product);
    this.sqlPkgDir = path.join(this.productDir, 'sql', opts.product);
    this.tracePath = opts.trace ? path.resolve(opts.trace) : undefined;
    this.evidence = [];
    this.notes = [];
    this.tools = this.loadTools();
  }

  loadTools() {
    const prodManifest = JSON.parse(
      fs.readFileSync(path.join(this.productDir, 'tools', 'tools.json'), 'utf8'),
    );
    const commonManifest = JSON.parse(
      fs.readFileSync(
        path.join(ROOT_DIR, 'products/common/tools/common_tools.json'),
        'utf8',
      ),
    );
    const all = [...prodManifest.tools, ...commonManifest.tools];
    return all.filter((t) => {
      if (t.kind === 'device' && !this.opts.adb) return false;
      return true;
    });
  }

  recordEvidence(tool, args, data, sql) {
    const id = `E${this.evidence.length + 1}`;
    this.evidence.push({id, tool, args, data, sql, atMs: Date.now()});
    return id;
  }

  runSql(sql, maxRows = 50) {
    if (!this.tracePath) {
      throw new Error('No --trace provided (or take_snapshot not called yet).');
    }
    const args = [];
    if (fs.existsSync(this.sqlPkgDir)) {
      args.push('--add-sql-package', this.sqlPkgDir);
    }
    args.push('-Q', `PERFETTO PRAGMA pipelines = 1;\n${sql}`, this.tracePath);
    const res = spawnSync(this.tpBin, args, {
      encoding: 'utf8',
      maxBuffer: 32 * 1024 * 1024,
    });
    if (res.status !== 0) {
      throw new Error((res.stderr || res.stdout || 'trace_processor failed').trim());
    }
    return parseCsvRows(res.stdout, maxRows);
  }

  callTool(name, args = {}) {
    const spec = this.tools.find((t) => t.name === name);
    if (!spec) {
      return {isError: true, output: {error: `Unknown tool "${name}"`}};
    }
    const scope = this.tracePath ? path.basename(this.tracePath) : 'headless';
    const baseParams = {start_ts: null, end_ts: null, process: null, ...args};

    try {
      if (spec.kind === 'sql' && spec.sql) {
        const bound = bindSqlParams(spec.sql, baseParams);
        const maxRows = typeof args.limit === 'number' ? args.limit : 50;
        const res = this.runSql(bound, maxRows);
        const eid = this.recordEvidence(name, args, res, bound);
        return {
          output: {
            evidence_id: eid,
            scope,
            row_count: res.rows.length,
            truncated: res.truncated,
            rows: res.rows,
          },
        };
      }

      if (spec.kind === 'sql_multi' && spec.queries) {
        const sections = {};
        const sqlLog = [];
        for (const [sec, rawSql] of Object.entries(spec.queries)) {
          const bound = bindSqlParams(rawSql, baseParams);
          sqlLog.push(`-- ${sec}\n${bound}`);
          try {
            sections[sec] = this.runSql(bound, 25).rows;
          } catch (e) {
            sections[sec] = {error: String(e.message || e)};
          }
        }
        const eid = this.recordEvidence(name, args, sections, sqlLog.join('\n\n'));
        return {output: {evidence_id: eid, scope, sections}};
      }

      if (spec.kind === 'sql_adhoc') {
        const sql = String(args.sql ?? '').trim();
        if (!sql) return {isError: true, output: {error: 'Missing sql'}};
        const maxRows = typeof args.max_rows === 'number' ? args.max_rows : 50;
        const res = this.runSql(sql, maxRows);
        const eid = this.recordEvidence(name, args, res, sql);
        return {
          output: {
            evidence_id: eid,
            scope,
            columns: res.columns,
            row_count: res.rows.length,
            truncated: res.truncated,
            rows: res.rows,
          },
        };
      }

      if (spec.kind === 'note') {
        const ids = Array.isArray(args.evidence_ids)
          ? args.evidence_ids.map(String)
          : [];
        const known = new Set(this.evidence.map((e) => e.id));
        const missing = ids.filter((id) => !known.has(id));
        const verified = ids.length > 0 && missing.length === 0;
        this.notes.push({
          title: String(args.title ?? ''),
          summary: String(args.summary ?? ''),
          evidenceIds: ids,
          verified,
        });
        return {
          output: {
            recorded: true,
            verified,
            missing_evidence_ids: missing,
            note_count: this.notes.length,
          },
        };
      }

      if (spec.kind === 'device') {
        if (name === 'device_status') {
          const out = execFileSync('adb', ['devices', '-l'], {encoding: 'utf8'});
          const eid = this.recordEvidence(name, args, {adb_devices: out.trim()});
          return {output: {evidence_id: eid, adb_devices: out.trim(), trace: this.tracePath ?? null}};
        }
        if (name === 'take_snapshot') {
          const cfgPath = path.join(this.productDir, 'configs/snapshot.pbtx');
          const cfg = fs.readFileSync(cfgPath, 'utf8') + '\nduration_ms: 5000\n';
          const remote = '/data/misc/perfetto-traces/sysmon_mcp_snap.pftrace';
          const local = path.join(os.tmpdir(), `sysmon_mcp_${Date.now()}.pftrace`);
          execFileSync('adb', ['shell', 'perfetto', '--txt', '-c', '-', '-o', remote], {
            input: cfg,
            stdio: ['pipe', 'pipe', 'pipe'],
          });
          execFileSync('adb', ['pull', remote, local]);
          this.tracePath = local;
          const eid = this.recordEvidence(name, args, {trace_path: local});
          return {output: {evidence_id: eid, snapshot_trace: local}};
        }
      }

      return {isError: true, output: {error: `Unsupported tool "${name}"`}};
    } catch (e) {
      return {isError: true, output: {error: e instanceof Error ? e.message : String(e)}};
    }
  }
}

function main() {
  const opts = parseArgs(process.argv.slice(2));
  const rt = new ProductMcpRuntime(opts);

  if (opts.listTools) {
    console.log(
      JSON.stringify(
        rt.tools.map((t) => ({
          name: t.name,
          kind: t.kind,
          description: t.description,
          inputSchema: t.parameters,
        })),
        null,
        2,
      ),
    );
    return;
  }

  if (opts.call) {
    const parsedArgs = JSON.parse(opts.callArgs || '{}');
    const res = rt.callTool(opts.call, parsedArgs);
    console.log(JSON.stringify(res.output, null, 2));
    if (res.isError) process.exit(1);
    return;
  }

  // Stdio JSON-RPC 2.0 MCP server loop
  const rl = readline.createInterface({input: process.stdin, terminal: false});
  const send = (msg) => process.stdout.write(`${JSON.stringify(msg)}\n`);

  rl.on('line', (line) => {
    const trimmed = line.trim();
    if (!trimmed) return;
    let req;
    try {
      req = JSON.parse(trimmed);
    } catch {
      return;
    }
    const {id, method, params} = req;
    if (method === 'initialize') {
      send({
        jsonrpc: '2.0',
        id,
        result: {
          protocolVersion: params?.protocolVersion ?? '2024-11-05',
          capabilities: {tools: {}},
          serverInfo: {name: `perfetto-${opts.product}-mcp`, version: '0.1.0'},
        },
      });
    } else if (method === 'notifications/initialized') {
      // No response required.
    } else if (method === 'tools/list') {
      send({
        jsonrpc: '2.0',
        id,
        result: {
          tools: rt.tools.map((t) => ({
            name: t.name,
            description: t.description,
            inputSchema: t.parameters,
          })),
        },
      });
    } else if (method === 'tools/call') {
      const res = rt.callTool(params?.name, params?.arguments ?? {});
      send({
        jsonrpc: '2.0',
        id,
        result: {
          content: [{type: 'text', text: JSON.stringify(res.output, null, 2)}],
          isError: Boolean(res.isError),
        },
      });
    } else if (id !== undefined) {
      send({
        jsonrpc: '2.0',
        id,
        error: {code: -32601, message: `Method not found: ${method}`},
      });
    }
  });
}

main();
