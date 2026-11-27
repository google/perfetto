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
import {assetSrc} from '../base/assets';
import type {SkillFile, SkillInfo, SkillManager} from '../public/skills';

// Limits on what a source can put in front of an agent. The index lines go
// into every prompt; the files go into the conversation history.
export const MAX_SKILLS = 50;
export const MAX_SKILL_FILE_BYTES = 64 * 1024;
export const MAX_DESCRIPTION_CHARS = 1024;

// How long a call waits for a skill index. A call that cannot wait gets no
// skills from that index; a later call gets them once the index arrives.
export const SKILL_INDEX_TIMEOUT_MS = 3000;

const SKILL_FILE = 'SKILL.md';
const NAME_RE = /^[a-z0-9]+(-[a-z0-9]+)*$/;
const PATH_SEGMENT_RE = /^[A-Za-z0-9_.-]+$/;

// The index format written by tools/gen_skill_bundle.py.
export const skillIndexSchema = z.object({
  skills: z.array(
    z.object({
      name: z.string(),
      description: z.string(),
      files: z.array(
        z.object({path: z.string(), bytes: z.number().int().nonnegative()}),
      ),
    }),
  ),
});
export type SkillIndex = z.infer<typeof skillIndexSchema>;

/** A place skills come from: one index plus a way to fetch each file. */
export interface SkillSource {
  loadIndex(): Promise<SkillIndex>;
  fetchFile(skill: string, path: string): Promise<string>;
}

/**
 * Normalises a skill-relative path. Returns undefined for anything that could
 * leave the skill root or is not a plain .md/.sql file.
 */
export function normalizeSkillPath(path: string): string | undefined {
  let p = path.trim();
  if (p.startsWith('$SKILL_ROOT/')) p = p.slice('$SKILL_ROOT/'.length);
  while (p.startsWith('./')) p = p.slice(2);
  if (p === '' || p.startsWith('/')) return undefined;
  const segments = p.split('/');
  for (const s of segments) {
    if (s === '' || s === '.' || s === '..' || !PATH_SEGMENT_RE.test(s)) {
      return undefined;
    }
  }
  if (!p.endsWith('.md') && !p.endsWith('.sql')) return undefined;
  return p;
}

/** Returns the markdown body of a SKILL.md, without its YAML frontmatter. */
export function stripFrontmatter(text: string): string {
  if (!text.startsWith('---\n')) return text;
  const end = text.indexOf('\n---\n', 4);
  return end < 0 ? text : text.slice(end + 5).replace(/^\n+/, '');
}

// Drops index entries that break the format rules, so one bad entry cannot
// take out the others.
function validEntries(index: SkillIndex): SkillIndex['skills'] {
  const out: SkillIndex['skills'] = [];
  for (const e of index.skills) {
    const problem = entryProblem(e);
    if (problem !== undefined) {
      console.warn(`Skills: dropping "${e.name}": ${problem}`);
      continue;
    }
    out.push(e);
  }
  return out;
}

function entryProblem(e: SkillIndex['skills'][number]): string | undefined {
  if (!NAME_RE.test(e.name) || e.name.length > 64) return 'invalid name';
  if (e.description === '' || e.description.length > MAX_DESCRIPTION_CHARS) {
    return 'invalid description';
  }
  if (!e.files.some((f) => f.path === SKILL_FILE)) return 'no SKILL.md';
  for (const f of e.files) {
    if (normalizeSkillPath(f.path) !== f.path) return `bad path ${f.path}`;
    if (f.bytes > MAX_SKILL_FILE_BYTES) return `${f.path} is too large`;
  }
  return undefined;
}

interface Entry {
  readonly info: SkillInfo;
  readonly paths: ReadonlySet<string>;
}

export class SkillManagerImpl implements SkillManager {
  private index?: Promise<SkillIndex>;
  private readonly fileCache = new Map<string, Promise<string>>();

  constructor(private readonly source: SkillSource) {}

  async list(): Promise<ReadonlyArray<SkillInfo>> {
    const entries = await this.entries();
    return Array.from(entries.values(), (e) => e.info);
  }

  async readFile(skill: string, path: string): Promise<string> {
    const entry = (await this.entries()).get(skill);
    if (entry === undefined) throw new Error(`Unknown skill "${skill}"`);
    const rel = normalizeSkillPath(path);
    if (rel === undefined || !entry.paths.has(rel)) {
      throw new Error(`"${path}" is not a file of skill "${skill}"`);
    }
    const key = `${skill}\0${rel}`;
    let text = this.fileCache.get(key);
    if (text === undefined) {
      text = this.source.fetchFile(skill, rel).then((t) => {
        if (new TextEncoder().encode(t).byteLength > MAX_SKILL_FILE_BYTES) {
          throw new Error(`${skill}/${rel} is too large`);
        }
        return rel === SKILL_FILE ? stripFrontmatter(t) : t;
      });
      // A failed fetch can be retried on the next call.
      text.catch(() => this.fileCache.delete(key));
      this.fileCache.set(key, text);
    }
    return text;
  }

  // The index is fetched once, on first use.
  private loadIndex(): Promise<SkillIndex> {
    if (this.index === undefined) {
      this.index = this.source.loadIndex().catch((e) => {
        console.warn(`Skills: cannot load the skill index: ${e}`);
        return {skills: []};
      });
    }
    return withTimeout(this.index, 'the skill index');
  }

  private async entries(): Promise<Map<string, Entry>> {
    const index = await this.loadIndex();
    const byName = new Map<string, Entry>();
    for (const e of validEntries(index)) {
      if (byName.has(e.name)) {
        console.warn(`Skills: "${e.name}" is a duplicate`);
        continue;
      }
      const files: SkillFile[] = e.files.map((f) => ({...f}));
      byName.set(e.name, {
        info: {name: e.name, description: e.description, files},
        paths: new Set(files.map((f) => f.path)),
      });
    }
    const sorted = Array.from(byName.keys()).sort();
    const limited = sorted.slice(0, MAX_SKILLS);
    if (sorted.length > MAX_SKILLS) {
      console.warn(`Skills: only the first ${MAX_SKILLS} skills are used`);
    }
    return new Map(limited.map((n) => [n, byName.get(n)!]));
  }
}

async function withTimeout(
  index: Promise<SkillIndex>,
  what: string,
): Promise<SkillIndex> {
  let timer: ReturnType<typeof setTimeout> | undefined;
  const timeout = new Promise<SkillIndex>((resolve) => {
    timer = setTimeout(() => {
      console.warn(`Skills: ${what} is not ready yet`);
      resolve({skills: []});
    }, SKILL_INDEX_TIMEOUT_MS);
  });
  try {
    return await Promise.race([index, timeout]);
  } finally {
    clearTimeout(timer);
  }
}

async function fetchOk(url: string): Promise<Response> {
  const resp = await fetch(url);
  if (!resp.ok) throw new Error(`${url}: HTTP ${resp.status}`);
  return resp;
}

/**
 * The skills built into this UI version by tools/gen_skill_bundle.py, served
 * next to the other assets (see generateUiSkills() in ui/build.mjs).
 */
export function createBuiltinSkillSource(): SkillSource {
  return {
    async loadIndex() {
      const resp = await fetch(assetSrc('ai_skills/index.json'));
      // A build without skills (e.g. some test setups) has no index.
      if (resp.status === 404) return {skills: []};
      if (!resp.ok) throw new Error(`HTTP ${resp.status}`);
      return skillIndexSchema.parse(await resp.json());
    },
    async fetchFile(skill: string, path: string) {
      const resp = await fetchOk(assetSrc(`ai_skills/${skill}/${path}`));
      return resp.text();
    },
  };
}
