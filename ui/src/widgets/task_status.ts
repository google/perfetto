// Copyright (C) 2025 The Android Open Source Project
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

import m from 'mithril';
import {taskTracker, TaskInfo} from '../base/task_tracker';
import {Tooltip} from './tooltip';
import {Icon} from './icon';

function formatElapsed(ms: number): string {
  if (ms < 1000) {
    return `${Math.round(ms)}ms`;
  }
  return `${(ms / 1000).toFixed(1)}s`;
}

/**
 * A small status indicator component that shows current task activity.
 *
 * - Shows task count with an icon.
 * - When tasks are in flight, shows a tooltip with task labels and elapsed times.
 */
export class TaskStatus implements m.ClassComponent {
  private unsubscribe?: () => void;

  oninit() {
    this.unsubscribe = taskTracker.subscribe(() => {
      m.redraw();
    });
  }

  onremove() {
    this.unsubscribe?.();
    this.unsubscribe = undefined;
  }

  view(): m.Children {
    const count = taskTracker.size;
    const tasks = taskTracker.tasks;

    const content = m(
      '.pf-task-status',
      m(Icon, {icon: 'pending_actions'}),
      m('span.pf-task-status__count', String(count)),
    );

    if (count === 0) {
      return content;
    }

    return m(Tooltip, {trigger: content}, this.renderTaskList(tasks));
  }

  private renderTaskList(tasks: TaskInfo[]): m.Children {
    return m(
      '.pf-task-status__list',
      tasks.map((task) =>
        m(
          '.pf-task-status__item',
          m('span.pf-task-status__label', task.label),
          m('span.pf-task-status__elapsed', formatElapsed(task.elapsed)),
        ),
      ),
    );
  }
}
