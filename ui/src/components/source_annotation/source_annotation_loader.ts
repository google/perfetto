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

import {sqliteString} from '../../base/string_utils';
import type {Engine} from '../../trace_processor/engine';
import {
  LONG,
  LONG_NULL,
  NUM,
  NUM_NULL,
  STR,
  STR_NULL,
} from '../../trace_processor/query_result';

// A function to annotate with sample counts. Identity comes from a callstack
// tree node: the mapping and a sampled address locate the bundled
// disassembly, the name is the fallback when no address is known.
export interface SourceAnnotationTarget {
  readonly functionName: string;
  readonly mappingName?: string;
  readonly mappingId?: number;
  // A sampled address in the function, relative to the mapping.
  readonly relPc?: bigint;
  readonly sourceFile?: string;
  // SQL subquery with a `callsite_id` column and one row per sample, scoping
  // the counts to the samples shown in the tree the function came from.
  readonly samplesSql: string;
}

export interface AnnotatedInstruction {
  readonly relPc: bigint;
  // Hex-encoded instruction bytes.
  readonly bytes: string;
  readonly text: string;
  readonly targetRelPc?: bigint;
  readonly targetSymbol?: string;
  readonly sourceFile?: string;
  readonly lineNumber?: number;
  readonly selfCount: number;
  readonly totalCount: number;
}

export interface AnnotatedLine {
  readonly lineNumber: number;
  readonly text: string;
  readonly selfCount: number;
  readonly totalCount: number;
}

export interface SourceAnnotation {
  readonly functionName: string;
  readonly sourceFile?: string;
  // Every line of `sourceFile` with its counts, or empty if the file was not
  // bundled with the trace.
  readonly lines: ReadonlyArray<AnnotatedLine>;
  // The function's instructions with their counts, or empty if no
  // disassembly was bundled for it.
  readonly instructions: ReadonlyArray<AnnotatedInstruction>;
  readonly maxLineSelf: number;
  readonly maxLineTotal: number;
  readonly maxInstructionSelf: number;
  readonly maxInstructionTotal: number;
}

export async function loadSourceAnnotation(
  engine: Engine,
  target: SourceAnnotationTarget,
): Promise<SourceAnnotation> {
  await engine.query('INCLUDE PERFETTO MODULE callstacks.annotate;');
  const functionId = await findFunction(engine, target);
  const instructions =
    functionId === undefined
      ? []
      : await loadInstructions(engine, target.samplesSql, functionId);
  const sourceFile = target.sourceFile ?? dominantSourceFile(instructions);
  const lines =
    sourceFile === undefined
      ? []
      : await loadLines(engine, target.samplesSql, sourceFile);
  return {
    functionName: target.functionName,
    sourceFile,
    lines,
    instructions,
    maxLineSelf: Math.max(0, ...lines.map((l) => l.selfCount)),
    maxLineTotal: Math.max(0, ...lines.map((l) => l.totalCount)),
    maxInstructionSelf: Math.max(0, ...instructions.map((i) => i.selfCount)),
    maxInstructionTotal: Math.max(0, ...instructions.map((i) => i.totalCount)),
  };
}

// The id in disassembly_function of the target, if its disassembly was
// bundled. Prefers the function containing the sampled address; falls back
// to matching the (possibly demangled) name within the mapping.
async function findFunction(
  engine: Engine,
  target: SourceAnnotationTarget,
): Promise<number | undefined> {
  const mappingFilter =
    target.mappingId !== undefined
      ? `m.id = ${target.mappingId}`
      : target.mappingName !== undefined
        ? `m.name = ${sqliteString(target.mappingName)}`
        : '1';
  const name = sqliteString(target.functionName);
  const containsRelPc =
    target.relPc !== undefined
      ? `${target.relPc} >= df.start_rel_pc AND ${target.relPc} < df.start_rel_pc + df.size`
      : '0';
  const result = await engine.query(`
    SELECT df.id AS id
    FROM disassembly_function df
    JOIN stack_profile_mapping m
      ON iif(df.build_id IS NOT NULL, df.build_id = m.build_id, df.path = m.name)
    WHERE ${mappingFilter}
      AND ((${containsRelPc}) OR df.name = ${name} OR demangle(df.name) = ${name})
    ORDER BY (${containsRelPc}) DESC
    LIMIT 1
  `);
  const it = result.iter({id: NUM});
  return it.valid() ? it.id : undefined;
}

async function loadInstructions(
  engine: Engine,
  samplesSql: string,
  functionId: number,
): Promise<AnnotatedInstruction[]> {
  const result = await engine.query(`
    SELECT
      rel_pc, bytes, text, target_rel_pc, target_symbol, source_file,
      line_number, self_count, total_count
    FROM _annotated_disassembly!((${samplesSql}), ${functionId})
  `);
  const instructions: AnnotatedInstruction[] = [];
  const it = result.iter({
    rel_pc: LONG,
    bytes: STR,
    text: STR,
    target_rel_pc: LONG_NULL,
    target_symbol: STR_NULL,
    source_file: STR_NULL,
    line_number: NUM_NULL,
    self_count: NUM,
    total_count: NUM,
  });
  for (; it.valid(); it.next()) {
    instructions.push({
      relPc: it.rel_pc,
      bytes: it.bytes,
      text: it.text,
      targetRelPc: it.target_rel_pc ?? undefined,
      targetSymbol: it.target_symbol ?? undefined,
      sourceFile: it.source_file ?? undefined,
      lineNumber: it.line_number ?? undefined,
      selfCount: it.self_count,
      totalCount: it.total_count,
    });
  }
  return instructions;
}

// The source file most of the function's instructions come from.
function dominantSourceFile(
  instructions: ReadonlyArray<AnnotatedInstruction>,
): string | undefined {
  const counts = new Map<string, number>();
  for (const insn of instructions) {
    if (insn.sourceFile === undefined || insn.sourceFile === '??') continue;
    counts.set(insn.sourceFile, (counts.get(insn.sourceFile) ?? 0) + 1);
  }
  let best: string | undefined;
  let bestCount = 0;
  for (const [file, count] of counts) {
    if (count > bestCount) {
      best = file;
      bestCount = count;
    }
  }
  return best;
}

async function loadLines(
  engine: Engine,
  samplesSql: string,
  sourceFile: string,
): Promise<AnnotatedLine[]> {
  const file = sqliteString(sourceFile);
  const contents = await engine.query(
    `SELECT contents FROM source_file WHERE path = ${file}`,
  );
  const contentsIt = contents.iter({contents: STR});
  if (!contentsIt.valid()) return [];
  const text = contentsIt.contents.split('\n');
  // A trailing newline does not start a new line.
  if (text.length > 0 && text[text.length - 1] === '') {
    text.pop();
  }

  const counts = await engine.query(`
    SELECT line_number, self_count, total_count
    FROM _sample_counts_by_source_line!((${samplesSql}))
    WHERE source_file = ${file}
  `);
  const selfCounts = new Map<number, number>();
  const totalCounts = new Map<number, number>();
  const it = counts.iter({line_number: NUM, self_count: NUM, total_count: NUM});
  for (; it.valid(); it.next()) {
    selfCounts.set(it.line_number, it.self_count);
    totalCounts.set(it.line_number, it.total_count);
  }
  return text.map((line, i) => ({
    lineNumber: i + 1,
    text: line,
    selfCount: selfCounts.get(i + 1) ?? 0,
    totalCount: totalCounts.get(i + 1) ?? 0,
  }));
}
