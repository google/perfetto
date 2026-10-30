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

import type {Engine} from '../../trace_processor/engine';
import type {SkillInfo, SkillManager} from '../../public/skills';
import {registerSkillTools, skillsPromptSection} from './skill_tools';
import {ToolRegistry} from './tools';

const PERFETTO: SkillInfo = {
  name: 'perfetto',
  description: 'Analyze Perfetto\n  traces.',
  files: [
    {path: 'SKILL.md', bytes: 10},
    {path: 'workflows/gpu/gpu_info.md', bytes: 20},
    {path: 'workflows/gpu/scripts/gpu_info.sql', bytes: 30},
  ],
};

function fakeSkills(): SkillManager & {reads: string[]} {
  const reads: string[] = [];
  return {
    reads,
    list: async () => [PERFETTO],
    readFile: async (skill, path) => {
      reads.push(`${skill}:${path}`);
      if (path.endsWith('.sql')) return 'SELECT 42 AS answer';
      if (path === 'SKILL.md') return '# Perfetto\nRead gpu_info.md.';
      return `contents of ${path}`;
    },
  };
}

// The tool callbacks only need engine.query() for run_skill_sql.
function fakeEngine(queries: string[]): Engine {
  return {
    query: async (sql: string) => {
      queries.push(sql);
      return {
        columns: () => ['answer'],
        iter: () => {
          let done = false;
          return {
            valid: () => !done,
            next: () => {
              done = true;
            },
            get: () => 42,
          };
        },
      };
    },
  } as unknown as Engine;
}

describe('skillsPromptSection', () => {
  test('lists one line per skill, sorted, with whitespace folded', () => {
    const section = skillsPromptSection([
      {...PERFETTO, name: 'zeta', description: 'Z.'},
      PERFETTO,
    ])!;
    const lines = section.split('\n');
    expect(lines[0]).toBe('Skills:');
    expect(lines[1]).toContain('call load_skill with its name');
    expect(lines.slice(2)).toEqual([
      '- perfetto: Analyze Perfetto traces.',
      '- zeta: Z.',
    ]);
  });
});

describe('skill tools', () => {
  test('load_skill returns SKILL.md and the file list', async () => {
    const reg = new ToolRegistry();
    const skills = fakeSkills();
    registerSkillTools(reg, skills, fakeEngine([]));
    const out = await reg.call('load_skill', {skill: 'perfetto'});
    expect(out).toContain('# Perfetto');
    expect(out).toContain('- workflows/gpu/gpu_info.md (20 bytes)');
    expect(out).not.toContain('- SKILL.md');
    await expect(reg.call('load_skill', {skill: 'nope'})).rejects.toThrow(
      'Unknown skill',
    );
  });

  test('run_skill_sql runs the script on the engine', async () => {
    const reg = new ToolRegistry();
    const queries: string[] = [];
    registerSkillTools(reg, fakeSkills(), fakeEngine(queries));
    const out = await reg.call('run_skill_sql', {
      skill: 'perfetto',
      path: 'workflows/gpu/scripts/gpu_info.sql',
    });
    expect(queries).toEqual(['SELECT 42 AS answer']);
    expect(JSON.parse(out)).toEqual({
      columns: ['answer'],
      rows: [{answer: 42}],
    });
    await expect(
      reg.call('run_skill_sql', {
        skill: 'perfetto',
        path: 'workflows/gpu/gpu_info.md',
      }),
    ).rejects.toThrow('only runs .sql files');
  });
});
