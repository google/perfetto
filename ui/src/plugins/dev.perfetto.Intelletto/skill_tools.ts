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

import {z} from 'zod';
import type {Engine} from '../../trace_processor/engine';
import type {SkillInfo, SkillManager} from '../../public/skills';
import {runQueryForModel} from './query';
import type {ToolRegistry} from './tools';

/**
 * The "Skills" section of the system prompt: one line per skill. This is the
 * only skill text that is always in context; everything else is loaded with
 * the tools below. Sorted by name so the prompt is stable across turns.
 */
export function skillsPromptSection(
  skills: ReadonlyArray<SkillInfo>,
): string | undefined {
  if (skills.length === 0) return undefined;
  const lines = [...skills]
    .sort((a, b) => a.name.localeCompare(b.name))
    .map((s) => `- ${s.name}: ${s.description.replace(/\s+/g, ' ').trim()}`);
  return [
    'Skills:',
    'Skills are guided workflows for common investigations. When the ' +
      "user's task matches a skill below, call load_skill with its name " +
      'before you answer, and follow it. Read the other files of a skill ' +
      'only when it points you to them.',
    ...lines,
  ].join('\n');
}

/**
 * The skills for the system prompt, or none if they cannot be listed. A slow
 * index is left out by the skill manager, so this does not block the
 * conversation.
 */
export async function listSkillsForPrompt(
  skills: SkillManager,
): Promise<ReadonlyArray<SkillInfo>> {
  try {
    return await skills.list();
  } catch {
    return [];
  }
}

/** Registers load_skill, read_skill_file and run_skill_sql. */
export function registerSkillTools(
  reg: ToolRegistry,
  skills: SkillManager,
  engine: Engine,
): void {
  reg.registerTool({
    name: 'load_skill',
    description:
      'Load a skill listed in the Skills section of the system prompt. ' +
      'Returns its SKILL.md instructions and the list of its other files.',
    shape: {
      skill: z.string().describe('The skill name from the Skills section.'),
    },
    callback: async ({skill}) => {
      const info = (await skills.list()).find((s) => s.name === skill);
      if (info === undefined) throw new Error(`Unknown skill "${skill}"`);
      const body = await skills.readFile(skill, 'SKILL.md');
      const files = info.files
        .filter((f) => f.path !== 'SKILL.md')
        .map((f) => `- ${f.path} (${f.bytes} bytes)`);
      return `${body}\n\nFiles of this skill (paths relative to the skill root):\n${files.join('\n')}`;
    },
  });

  reg.registerTool({
    name: 'read_skill_file',
    description:
      'Read one file of a loaded skill (a workflow, a reference or an SQL ' +
      'script). Use this when SKILL.md or another skill file points to it.',
    shape: {
      skill: z.string().describe('The skill name.'),
      path: z
        .string()
        .describe(
          'Path relative to the skill root; a $SKILL_ROOT/ prefix is fine.',
        ),
    },
    callback: async ({skill, path}) => skills.readFile(skill, path),
  });

  reg.registerTool({
    name: 'run_skill_sql',
    description:
      'Run an .sql script of a skill on the open trace and return the rows ' +
      'of its last statement (row-capped). Use this wherever a skill says ' +
      'to run `trace_processor query ... --query-file <path>`.',
    shape: {
      skill: z.string().describe('The skill name.'),
      path: z
        .string()
        .describe('Path of the .sql file relative to the skill root.'),
    },
    callback: async ({skill, path}) => {
      if (!path.trim().endsWith('.sql')) {
        throw new Error('run_skill_sql only runs .sql files');
      }
      const sql = await skills.readFile(skill, path);
      return runQueryForModel(engine, sql);
    },
  });
}
