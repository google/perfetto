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

// The index format written by tools/gen_skill_bundle.py: the builtin
// ai_skills/index.json and the extension-server modules/{id}/skills file.
export const skillIndexSchema = z.object({
  skills: z.array(
    z.object({
      name: z.string(),
      description: z.string(),
      files: z.array(
        z.object({path: z.string(), bytes: z.number().int().nonnegative()}),
      ),
      // Extension servers only: the name of a builtin skill that this skill
      // takes the place of. Honoured only from the embedder-managed server.
      replaces: z.string().optional(),
    }),
  ),
});
export type SkillIndex = z.infer<typeof skillIndexSchema>;

/** An index as loaded by a source. */
export interface LoadedSkillIndex extends SkillIndex {
  /**
   * The source's namespace (e.g. an extension server's 'com.acme'). Skills of
   * a namespaced source are listed as '<namespace>/<name>'. Builtin skills
   * have no namespace and keep their bare name.
   */
  readonly namespace?: string;
}

/** A place skills come from: one index plus a way to fetch each file. */
export interface SkillSource {
  /** Shown as SkillInfo.source. */
  readonly id: string;
  /**
   * True for the server that the UI build itself configures (the embedder).
   * Only such a source may replace a builtin skill: the deployer of a UI
   * build can ship its own build of a skill, a user-added server cannot.
   */
  readonly embedderManaged?: boolean;
  loadIndex(): Promise<LoadedSkillIndex>;
  /** `skill` is the name in the source's own index, not the listed id. */
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
function validEntries(
  index: SkillIndex,
  sourceId: string,
): SkillIndex['skills'] {
  const out: SkillIndex['skills'] = [];
  for (const e of index.skills) {
    const problem = entryProblem(e);
    if (problem !== undefined) {
      console.warn(`Skills: dropping "${e.name}" from ${sourceId}: ${problem}`);
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
  readonly source: SkillSource;
  // The skill's name in its source's index (info.name may differ).
  readonly nameInSource: string;
  readonly paths: ReadonlySet<string>;
  readonly replacesBuiltin: boolean;
}

export class SkillManagerImpl implements SkillManager {
  private readonly sources: SkillSource[];
  private readonly indexes = new Map<SkillSource, Promise<LoadedSkillIndex>>();
  private readonly fileCache = new Map<string, Promise<string>>();

  constructor(source: SkillSource) {
    this.sources = [source];
  }

  addSource(source: SkillSource): void {
    this.sources.push(source);
  }

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
    // Sources are only appended, so a source's position identifies it.
    const src = this.sources.indexOf(entry.source);
    const key = `${src}\0${entry.nameInSource}\0${rel}`;
    let text = this.fileCache.get(key);
    if (text === undefined) {
      text = entry.source.fetchFile(entry.nameInSource, rel).then((t) => {
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

  // Each source's index is fetched once, on first use.
  private loadIndex(source: SkillSource): Promise<LoadedSkillIndex> {
    let index = this.indexes.get(source);
    if (index === undefined) {
      index = source.loadIndex().catch((e) => {
        console.warn(`Skills: cannot load the index of ${source.id}: ${e}`);
        return {skills: []};
      });
      this.indexes.set(source, index);
    }
    return withTimeout(index, `the index of ${source.id}`);
  }

  private async entries(): Promise<Map<string, Entry>> {
    const indexes = await Promise.all(
      this.sources.map((s) => this.loadIndex(s)),
    );
    const byName = new Map<string, Entry>();
    this.sources.forEach((source, i) => {
      const index = indexes[i]!;
      for (const e of validEntries(index, source.id)) {
        const id = listedId(source, index, e);
        if (id === undefined) continue;
        const replacesBuiltin =
          index.namespace !== undefined && id === e.replaces;
        const existing = byName.get(id);
        // A replacing skill wins over a builtin one, whatever the source
        // order; otherwise the first source to list a name keeps it.
        if (
          existing !== undefined &&
          (existing.replacesBuiltin || !replacesBuiltin)
        ) {
          console.warn(`Skills: "${id}" from ${source.id} is a duplicate`);
          continue;
        }
        const files: SkillFile[] = e.files.map((f) => ({
          path: f.path,
          bytes: f.bytes,
        }));
        byName.set(id, {
          info: {
            name: id,
            description: e.description,
            files,
            source: source.id,
          },
          source,
          nameInSource: e.name,
          paths: new Set(files.map((f) => f.path)),
          replacesBuiltin,
        });
      }
    });
    // The cap keeps the earlier sources, so the builtin skills stay.
    const names = Array.from(byName.keys());
    if (names.length > MAX_SKILLS) {
      console.warn(`Skills: only the first ${MAX_SKILLS} skills are used`);
    }
    const limited = names.slice(0, MAX_SKILLS).sort();
    return new Map(limited.map((n) => [n, byName.get(n)!]));
  }
}

// The id under which a source's skill is listed, or undefined to drop it.
function listedId(
  source: SkillSource,
  index: LoadedSkillIndex,
  e: SkillIndex['skills'][number],
): string | undefined {
  if (index.namespace === undefined) return e.name;
  if (e.replaces !== undefined) {
    if (source.embedderManaged === true && NAME_RE.test(e.replaces)) {
      return e.replaces;
    }
    console.warn(
      `Skills: ignoring "replaces" of "${e.name}" from ${source.id}: ` +
        'only the embedder-managed server can replace a builtin skill',
    );
  }
  return `${index.namespace}/${e.name}`;
}

async function withTimeout(
  index: Promise<LoadedSkillIndex>,
  what: string,
): Promise<LoadedSkillIndex> {
  let timer: ReturnType<typeof setTimeout> | undefined;
  const timeout = new Promise<LoadedSkillIndex>((resolve) => {
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
    id: 'builtin',
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
