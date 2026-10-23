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

// Names of targets, and the specs that users type in the target menu, e.g.
// "worker/3" for task 3 of the job whose short name is "worker". The server
// only knows ids: "<id>" and "<id>/<task index>".

import {errResult, okResult, type Result} from '../../base/result';
import type {Target} from './wire';

/**
 * Validates a server spec and normalizes it, e.g.
 * " job-1/worker/03" -> "job-1/worker/3".
 *
 * A spec is a target id, e.g. "job-1/worker", or "<id>/<task index>" with
 * the index below the target's task count. Ids may contain '/', so an exact
 * id match wins. The target's own task is the target, e.g. "job-1/worker/0"
 * -> "job-1/worker".
 */
export function parseTargetSpec(
  spec: string,
  targets: readonly Target[],
): Result<string> {
  const trimmed = spec.trim();
  if (targets.some((t) => t.id === trimmed)) {
    return okResult(trimmed);
  }

  const slash = trimmed.lastIndexOf('/');
  const id = trimmed.slice(0, slash);
  const index = trimmed.slice(slash + 1);
  const target = slash < 0 ? undefined : targets.find((t) => t.id === id);
  if (target === undefined) {
    return errResult(`Unknown target "${trimmed}"`);
  }
  const normalized = taskSpec(target, index);
  if (normalized === undefined) {
    return errResult(
      `Task index must be an integer in [0, ${target.task_count}) for "${id}"`,
    );
  }
  return okResult(normalized);
}

/**
 * The spec of task `index` of `target`, e.g. "job-1/worker/3", or undefined
 * if there is no such task. The target's own task is the target itself.
 */
export function taskSpec(target: Target, index: string): string | undefined {
  const trimmed = index.trim();
  if (!/^\d+$/.test(trimmed)) return undefined;
  const n = Number(trimmed);
  if (n >= target.task_count) return undefined;
  return n === target.task_index ? target.id : `${target.id}/${n}`;
}

/** Whether `target` is a task of a job, even the job's only task. */
export function isIndexed(target: Target): boolean {
  return target.task_count > 0;
}

/** Short name of a target without its task, e.g. "worker". */
export function targetBaseName(target: Target): string {
  return target.short_name || target.id;
}

/** Short name of a target, with the index of a task, e.g. "worker/3". */
export function targetShortName(target: Target): string {
  const base = targetBaseName(target);
  if (target.short_name === '' || !isIndexed(target)) return base;
  return `${base}/${target.task_index}`;
}

/** Secondary text of a target: its description, or else its address. */
export function targetDescription(target: Target): string {
  return target.description || target.address;
}

/**
 * Label of a target, e.g. "worker/3 — Training job · worker (8 tasks)".
 */
export function targetLabel(target: Target): string {
  const description = target.description ? ` — ${target.description}` : '';
  const tasks = target.task_count > 1 ? ` (${target.task_count} tasks)` : '';
  return `${targetShortName(target)}${description}${tasks}`;
}

/**
 * The link of a target if it is a web link: the server is trusted, but not
 * with script URLs.
 */
export function targetLink(target: Target): string | undefined {
  return /^https?:\/\//.test(target.link) ? target.link : undefined;
}

/**
 * The targets that match `query`, ignoring case: by short name, id,
 * description or address. "<name>/<index>" also matches the tasks named, or
 * ending with, "<name>".
 */
export function filterTargets(
  targets: readonly Target[],
  query: string,
): Target[] {
  const q = query.trim().toLowerCase();
  if (q === '') return [...targets];
  const slash = q.indexOf('/');
  const base = slash < 0 ? '' : q.slice(0, slash);
  return targets.filter((t) => {
    const texts = [targetShortName(t), t.id, targetDescription(t), t.address];
    if (texts.some((text) => text.toLowerCase().includes(q))) return true;
    return base !== '' && isIndexed(t) && matchesBase(t, base);
  });
}

/** Whether `query` names a target exactly, ignoring case. */
export function hasExactTarget(
  targets: readonly Target[],
  query: string,
): boolean {
  return findExactTarget(targets, query) !== undefined;
}

/**
 * The server spec of a target typed by the user, or undefined if it names
 * none: a short name ("worker/0"), base name ("worker"), id or address, a
 * server spec ("job-1/worker/3"), or "<name>/<index>" for task <index> of
 * the job named, or ending with, "<name>".
 */
export function resolveTargetSpec(
  targets: readonly Target[],
  query: string,
): string | undefined {
  const exact = findExactTarget(targets, query);
  if (exact !== undefined) return exact.id;

  const spec = parseTargetSpec(query, targets);
  if (spec.ok) return spec.value;

  const match = /^([a-z0-9_.-]+)\/([0-9]+)$/.exec(query.trim().toLowerCase());
  if (match === null) return undefined;
  const base = match[1]!;
  const index = match[2]!;
  for (const t of targets) {
    if (!isIndexed(t) || !matchesBase(t, base)) continue;
    const task = taskSpec(t, index);
    if (task !== undefined) return task;
  }
  return undefined;
}

function findExactTarget(
  targets: readonly Target[],
  query: string,
): Target | undefined {
  const q = query.trim().toLowerCase();
  if (q === '') return undefined;
  return targets.find((t) =>
    [targetShortName(t), targetBaseName(t), t.id, t.address].some(
      (text) => text.toLowerCase() === q,
    ),
  );
}

// Whether the base name of `target` is, or ends with, `base` (lower case).
function matchesBase(target: Target, base: string): boolean {
  return targetBaseName(target).toLowerCase().endsWith(base);
}
