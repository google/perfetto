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
  type LoadedSkillIndex,
  type SkillIndex,
  type SkillSource,
} from './skill_manager';

function makeSource(
  index: SkillIndex,
  files: Record<string, string>,
  id = 'builtin',
): SkillSource & {fetches: string[]} {
  const fetches: string[] = [];
  return {
    id,
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
      id: 'builtin',
      loadIndex: async () => {
        throw new Error('offline');
      },
      fetchFile: async () => '',
    });
    expect(await mgr.list()).toEqual([]);
  });

  test('a late source is left out until its index arrives', async () => {
    vi.useFakeTimers();
    try {
      let arrive = (_: LoadedSkillIndex) => {};
      const mgr = new SkillManagerImpl(makeSource(PERFETTO, PERFETTO_FILES));
      mgr.addSource({
        id: 'server',
        loadIndex: () => new Promise((resolve) => (arrive = resolve)),
        fetchFile: async () => '',
      });
      const early = mgr.list();
      await vi.advanceTimersByTimeAsync(SKILL_INDEX_TIMEOUT_MS);
      expect((await early).map((s) => s.name)).toEqual(['perfetto']);
      arrive({...PERFETTO, namespace: 'com.acme'});
      expect((await mgr.list()).map((s) => s.name)).toEqual([
        'com.acme/perfetto',
        'perfetto',
      ]);
    } finally {
      vi.useRealTimers();
    }
  });

  test('lists namespaced sources as <namespace>/<name>', async () => {
    const mgr = new SkillManagerImpl(makeSource({skills: []}, {}));
    const src = makeSource(PERFETTO, PERFETTO_FILES, 'server');
    mgr.addSource({
      ...src,
      loadIndex: async () => ({...PERFETTO, namespace: 'com.acme'}),
    });
    const list = await mgr.list();
    expect(list.map((s) => s.name)).toEqual(['com.acme/perfetto']);
    expect(await mgr.readFile('com.acme/perfetto', 'SKILL.md')).toBe(
      '# Perfetto\n',
    );
    expect(src.fetches).toEqual(['perfetto/SKILL.md']);
  });

  test('only an embedder-managed source can replace a builtin skill', async () => {
    const replacing: SkillIndex = {
      skills: [{...PERFETTO.skills[0]!, replaces: 'perfetto'}],
    };
    const serverFiles = {'perfetto/SKILL.md': '# Server build\n'};

    const user = new SkillManagerImpl(makeSource(PERFETTO, PERFETTO_FILES));
    user.addSource({
      ...makeSource(replacing, serverFiles, 'user'),
      loadIndex: async () => ({...replacing, namespace: 'com.acme'}),
    });
    expect((await user.list()).map((s) => s.name)).toEqual([
      'com.acme/perfetto',
      'perfetto',
    ]);

    const embedder = new SkillManagerImpl(makeSource(PERFETTO, PERFETTO_FILES));
    embedder.addSource({
      ...makeSource(replacing, serverFiles, 'embedder'),
      embedderManaged: true,
      loadIndex: async () => ({...replacing, namespace: 'com.meta'}),
    });
    const list = await embedder.list();
    expect(list.map((s) => [s.name, s.source])).toEqual([
      ['perfetto', 'embedder'],
    ]);
    expect(await embedder.readFile('perfetto', 'SKILL.md')).toBe(
      '# Server build\n',
    );
  });

  test('falls back to the builtin skill when the replacing source fails', async () => {
    const mgr = new SkillManagerImpl(makeSource(PERFETTO, PERFETTO_FILES));
    mgr.addSource({
      id: 'embedder',
      embedderManaged: true,
      loadIndex: async () => {
        throw new Error('403');
      },
      fetchFile: async () => '',
    });
    const list = await mgr.list();
    expect(list.map((s) => [s.name, s.source])).toEqual([
      ['perfetto', 'builtin'],
    ]);
  });
});
