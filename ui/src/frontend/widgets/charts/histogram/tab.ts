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
import {DetailsShell} from '../../../../widgets/details_shell';
import {filterTitle} from '../../../widgets/sql/table/column';
import {addEphemeralTab} from '../../../../common/add_ephemeral_tab';
import {Tab} from '../../../../public/tab';
import {Histogram, HistogramConfig} from './histogram';
import {toTitleCase} from '../chart';

export function addHistogramTab(config: HistogramConfig): void {
  addEphemeralTab('histogramTab', new HistogramTab(config));
}

export class HistogramTab implements Tab {
  constructor(private readonly config: HistogramConfig) {}

  render() {
    return m(
      DetailsShell,
      {
        title: this.getTitle(),
        description: this.getDescription(),
      },
      m(Histogram, this.config),
    );
  }

  getTitle(): string {
    return `${toTitleCase(this.config.columnTitle)} Histogram`;
  }

  private getDescription(): string {
    let desc = `Count distribution for ${this.config.tableDisplay ?? ''} table`;

    if (this.config.filters && this.config.filters.length > 0) {
      desc += ' where ';
      desc += this.config.filters.map((f) => filterTitle(f)).join(', ');
    }

    return desc;
  }
}
