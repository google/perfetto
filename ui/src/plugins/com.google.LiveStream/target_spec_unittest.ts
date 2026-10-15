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

import {okResult} from '../../base/result';
import {
  filterTargets,
  hasExactTarget,
  parseTargetSpec,
  resolveTargetSpec,
  targetLabel,
  targetLink,
  targetShortName,
} from './target_spec';
import {TARGET} from './wire';

const WORKER = TARGET.parse({
  id: 'job-1/worker',
  short_name: 'worker',
  description: 'Training job · worker',
  task_count: 8,
});

// A target whose id looks like a task of another target.
const EVAL_TASK = TARGET.parse({id: 'job-1/eval/2', task_count: 1});

const TARGETS = [WORKER, EVAL_TASK];

test('accepts target ids', () => {
  expect(parseTargetSpec(' job-1/worker ', TARGETS)).toEqual(
    okResult('job-1/worker'),
  );
  expect(parseTargetSpec('job-1/eval/2', TARGETS)).toEqual(
    okResult('job-1/eval/2'),
  );
});

test('accepts tasks of targets', () => {
  expect(parseTargetSpec('job-1/worker/07', TARGETS)).toEqual(
    okResult('job-1/worker/7'),
  );
});

test('maps the own task of a target to the target', () => {
  expect(parseTargetSpec('job-1/worker/0', TARGETS)).toEqual(
    okResult('job-1/worker'),
  );
});

test('rejects unknown targets and tasks', () => {
  const invalid = [
    '',
    'job-1',
    'job-2/worker',
    'job-1/eval',
    'job-1/worker/',
    'job-1/worker/8',
    'job-1/worker/-1',
    'job-1/worker/1.5',
  ];
  for (const spec of invalid) {
    expect(parseTargetSpec(spec, TARGETS).ok).toBe(false);
  }
});

test('targetShortName shows the index of tasks, even of one-task jobs', () => {
  expect(targetShortName(WORKER)).toBe('worker/0');
  expect(targetShortName({...WORKER, task_index: 3})).toBe('worker/3');
  expect(targetShortName({...WORKER, task_count: 1})).toBe('worker/0');
  expect(targetShortName({...WORKER, task_count: 0})).toBe('worker');
  expect(targetShortName(EVAL_TASK)).toBe('job-1/eval/2');
});

test('targetLabel', () => {
  expect(targetLabel(WORKER)).toBe(
    'worker/0 — Training job · worker (8 tasks)',
  );
  expect(targetLabel({...WORKER, task_count: 1})).toBe(
    'worker/0 — Training job · worker',
  );
  expect(targetLabel(EVAL_TASK)).toBe('job-1/eval/2');
});

test('filterTargets matches names, ids, descriptions and addresses', () => {
  const trainer = TARGET.parse({
    id: 'job-2/trainer',
    short_name: 'trainer',
    address: '/cluster/x/trainer',
    task_count: 4,
  });
  const targets = [WORKER, EVAL_TASK, trainer];

  expect(filterTargets(targets, ' ')).toEqual(targets);
  expect(filterTargets(targets, 'job-1')).toEqual([WORKER, EVAL_TASK]);
  expect(filterTargets(targets, 'TRAINING')).toEqual([WORKER]);
  expect(filterTargets(targets, 'cluster/x')).toEqual([trainer]);
  // Tasks of multi-task targets named, or ending with, the name.
  expect(filterTargets(targets, 'trainer/3')).toEqual([trainer]);
  expect(filterTargets(targets, 'ainer/3')).toEqual([trainer]);
});

test('resolveTargetSpec resolves names, ids and tasks', () => {
  expect(resolveTargetSpec(TARGETS, 'worker')).toBe('job-1/worker');
  expect(resolveTargetSpec(TARGETS, 'Worker/0')).toBe('job-1/worker');
  expect(resolveTargetSpec(TARGETS, 'worker/5')).toBe('job-1/worker/5');
  expect(resolveTargetSpec(TARGETS, 'job-1/worker/5')).toBe('job-1/worker/5');
  expect(resolveTargetSpec(TARGETS, 'job-1/eval/2')).toBe('job-1/eval/2');

  for (const unknown of ['', 'nope', 'worker/8', 'eval/1']) {
    expect(resolveTargetSpec(TARGETS, unknown)).toBeUndefined();
  }
});

test('resolveTargetSpec resolves the only task of a job', () => {
  const trainer = TARGET.parse({
    id: 'job-2/trainer',
    short_name: 'trainer',
    task_count: 1,
  });
  const targets = [WORKER, trainer];

  expect(resolveTargetSpec(targets, 'trainer/0')).toBe('job-2/trainer');
  expect(resolveTargetSpec(targets, 'ainer/0')).toBe('job-2/trainer');
  expect(resolveTargetSpec(targets, 'trainer')).toBe('job-2/trainer');
  expect(resolveTargetSpec(targets, 'trainer/1')).toBeUndefined();
});

test('hasExactTarget ignores case, not missing characters', () => {
  expect(hasExactTarget(TARGETS, 'WORKER')).toBe(true);
  expect(hasExactTarget(TARGETS, 'work')).toBe(false);
});

test('targetLink accepts web links only', () => {
  expect(targetLink({...WORKER, link: 'https://example.com/w'})).toBe(
    'https://example.com/w',
  );
  expect(targetLink({...WORKER, link: 'javascript:alert(1)'})).toBeUndefined();
});
