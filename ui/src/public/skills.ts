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

/** One file of a skill, as listed in its index. */
export interface SkillFile {
  /** Path relative to the skill root, e.g. 'workflows/gpu/gpu_info.md'. */
  readonly path: string;
  readonly bytes: number;
}

/**
 * The index entry of an agent skill (the Agent Skills format: a SKILL.md plus
 * the files it references). Listing skills is cheap: only the name and the
 * description go into an agent's prompt; the files are read on demand.
 */
export interface SkillInfo {
  /** The id an agent passes to readFile(). */
  readonly name: string;
  /** When to use the skill. This is what an agent routes on. */
  readonly description: string;
  readonly files: ReadonlyArray<SkillFile>;
  /** Where the skill comes from, e.g. 'builtin'. */
  readonly source: string;
}

/**
 * The skills available to in-UI agents. Skills built into the UI are always
 * present; other sources can add more.
 */
export interface SkillManager {
  /** The index of every available skill, sorted by name. */
  list(): Promise<ReadonlyArray<SkillInfo>>;

  /**
   * Reads one file of a skill. `path` is relative to the skill root; a
   * leading `$SKILL_ROOT/` or `./` is accepted. SKILL.md is returned without
   * its frontmatter. Rejects if the file is not in the skill's index.
   */
  readFile(skill: string, path: string): Promise<string>;
}
