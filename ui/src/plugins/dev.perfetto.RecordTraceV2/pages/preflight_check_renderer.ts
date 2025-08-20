// Copyright (C) 2024 The Android Open Source Project
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
import {
  PreflightCheck,
  PreflightCheckResult,
  WithPreflightChecks,
} from '../interfaces/connection_check';
import {Spinner} from '../../../widgets/spinner';
import {Icon} from '../../../widgets/icon';
import {linkify} from '../../../widgets/anchor';

type PreflightCheckWithResult = PreflightCheck & {
  result?: PreflightCheckResult;
};

export class PreflightCheckRenderer {
  private results = new Array<PreflightCheckWithResult>();
  private allChecksCompleted = false;
  private numChecksFailed = 0;

  constructor(private testTarget: WithPreflightChecks) {}

  async runPreflightChecks(): Promise<boolean> {
    this.allChecksCompleted = false;
    this.numChecksFailed = 0;
    for await (const check of this.testTarget.runPreflightChecks()) {
      const entry: PreflightCheckWithResult = {...check, result: check.status};
      this.results.push(entry);
      this.numChecksFailed += check.status.ok ? 0 : 1;
      m.redraw();
    }
    this.allChecksCompleted = true;
    m.redraw();
    return this.numChecksFailed === 0;
  }

  renderIcon(): m.Children {
    const attrs = {filled: true, className: 'preflight-checks-icon'};
    if (!this.allChecksCompleted) {
      return m(Spinner);
    }
    if (this.numChecksFailed > 0) {
      attrs.className += ' ok';
      return m(Icon, {icon: 'report', ...attrs});
    }
    attrs.className += ' error';
    return m(Icon, {icon: 'check_circle', ...attrs});
  }

  renderTable(): m.Children {
    return m(
      'table.preflight-checks-table',
      this.results.map((res) =>
        m(
          'tr',
          m('td', res.name),
          m(
            'td',
            res.result === undefined
              ? m(Spinner)
              : res.result.ok
                ? m('span.ok', linkify(res.result.value))
                : m('span.error', linkify(res.result.error)),
            res.remediation && m('div', m(res.remediation)),
          ),
        ),
      ),
    );
  }
}
