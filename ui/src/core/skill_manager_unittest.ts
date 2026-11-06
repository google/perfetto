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

import {
  normalizeSkillPath,
  SKILL_INDEX_TIMEOUT_MS,
  SkillManagerImpl,
  type SkillIndex,
  type SkillSource,
} from './skill_manager';

function makeSource(
  index: SkillIndex,
  files: Record<string, string>,
): SkillSource & {fetches: string[]} {
  const fetches: string[] = [];
  return {
    fetches,
    loadIndex: async () => index,
    fetchFile: async (skill, path) => {
      fetches.push(`${skill}/${path}`);
      const text = files[`${skill}/${path}`];
      if (text === undefined) throw new Error('404');
      return text;
    },
  };
}

const PERFETTO: SkillIndex = {
  skills: [
    {
      name: 'perfetto',
      description: 'Analyze Perfetto traces.',
      files: [
        {path: 'SKILL.md', bytes: 40},
        {path: 'workflows/gpu/gpu_info.md', bytes: 10},
        {path: 'workflows/gpu/scripts/gpu_info.sql', bytes: 9},
      ],
    },
  ],
};

const PERFETTO_FILES = {
  'perfetto/SKILL.md': '---\nname: perfetto\n---\n\n# Perfetto\n',
  'perfetto/workflows/gpu/gpu_info.md': '# GPU info\n',
  'perfetto/workflows/gpu/scripts/gpu_info.sql': 'SELECT 1;\n',
};

describe('normalizeSkillPath', () => {
  test('keeps .md/.sql paths inside the skill and rejects the rest', () => {
    expect(normalizeSkillPath('$SKILL_ROOT/workflows/x.md')).toBe(
      'workflows/x.md',
    );
    expect(normalizeSkillPath('./a/b.sql')).toBe('a/b.sql');
    expect(normalizeSkillPath('../x.md')).toBeUndefined();
    expect(normalizeSkillPath('a/../../x.md')).toBeUndefined();
    expect(normalizeSkillPath('/etc/x.md')).toBeUndefined();
    expect(normalizeSkillPath('scripts/x.py')).toBeUndefined();
  });
});

describe('SkillManagerImpl', () => {
  test('reads files lazily, strips SKILL.md frontmatter and caches', async () => {
    const src = makeSource(PERFETTO, PERFETTO_FILES);
    const mgr = new SkillManagerImpl(src);
    await mgr.list();
    expect(src.fetches).toEqual([]);

    expect(await mgr.readFile('perfetto', 'SKILL.md')).toBe('# Perfetto\n');
    expect(
      await mgr.readFile('perfetto', '$SKILL_ROOT/workflows/gpu/gpu_info.md'),
    ).toBe('# GPU info\n');
    await mgr.readFile('perfetto', 'workflows/gpu/gpu_info.md');
    expect(src.fetches).toEqual([
      'perfetto/SKILL.md',
      'perfetto/workflows/gpu/gpu_info.md',
    ]);
  });

  test('rejects unknown skills and files not in the index', async () => {
    const mgr = new SkillManagerImpl(makeSource(PERFETTO, PERFETTO_FILES));
    await expect(mgr.readFile('nope', 'SKILL.md')).rejects.toThrow(
      'Unknown skill',
    );
    await expect(
      mgr.readFile('perfetto', 'workflows/other.md'),
    ).rejects.toThrow('not a file of skill');
    await expect(mgr.readFile('perfetto', '../x.md')).rejects.toThrow(
      'not a file of skill',
    );
  });

  test('drops invalid entries', async () => {
    const mgr = new SkillManagerImpl(
      makeSource(
        {
          skills: [
            ...PERFETTO.skills,
            {name: 'Bad Name', description: 'x', files: []},
            {name: 'no-skill-md', description: 'x', files: []},
            {
              name: 'escapes',
              description: 'x',
              files: [
                {path: 'SKILL.md', bytes: 1},
                {path: '../x.md', bytes: 1},
              ],
            },
          ],
        },
        PERFETTO_FILES,
      ),
    );
    expect((await mgr.list()).map((s) => s.name)).toEqual(['perfetto']);
  });

  test('lists no skills when the index cannot be loaded', async () => {
    const mgr = new SkillManagerImpl({
      loadIndex: async () => {
        throw new Error('offline');
      },
      fetchFile: async () => '',
    });
    expect(await mgr.list()).toEqual([]);
  });

  test('lists no skills while the index is late, then lists them', async () => {
    vi.useFakeTimers();
    try {
      let arrive = (_: SkillIndex) => {};
      const mgr = new SkillManagerImpl({
        loadIndex: () => new Promise((resolve) => (arrive = resolve)),
        fetchFile: async () => '',
      });
      const early = mgr.list();
      await vi.advanceTimersByTimeAsync(SKILL_INDEX_TIMEOUT_MS);
      expect(await early).toEqual([]);
      arrive(PERFETTO);
      expect((await mgr.list()).map((s) => s.name)).toEqual(['perfetto']);
    } finally {
      vi.useRealTimers();
    }
  });
});
