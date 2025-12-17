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
import {SearchManagerImpl} from '../../core/search_manager';
import {Trace} from '../../public/trace';
import {Anchor} from '../../widgets/anchor';
import {DetailsShell} from '../../widgets/details_shell';
import {DataGrid} from '../../components/widgets/datagrid/datagrid';
import {Row} from '../../trace_processor/query_result';
import {SchemaRegistry} from '../../components/widgets/datagrid/datagrid_schema';

interface TabAttrs {
  trace: Trace;
}

export class SearchResultsTab implements m.ClassComponent<TabAttrs> {
  view({attrs}: m.CVnode<TabAttrs>) {
    const trace = attrs.trace;
    const searchManager = trace.search as SearchManagerImpl;
    const searchResults = searchManager.searchResults;
    const searchText = searchManager.searchText;

    const schema: SchemaRegistry = {
      data: {
        id: {
          title: 'Event ID',
          cellRenderer: (value, row) => {
            if (typeof row.trackUri === 'string') {
              return m(
                Anchor,
                {
                  onclick: () => {
                    trace.selection.selectTrackEvent(
                      row.trackUri as string,
                      value as number,
                      {
                        switchToCurrentSelectionTab: false,
                        clearSearch: false,
                        scrollToSelection: true,
                      },
                    );
                  },
                },
                String(value),
              );
            }
            return String(value);
          },
        },
        ts: {
          title: 'Timestamp',
        },
        trackUri: {
          title: 'Track URI',
        },
      },
    };

    const rowData: Row[] = [];

    if (searchResults) {
      for (let i = 0; i < searchResults.totalResults; i++) {
        const eventId = searchResults.eventIds[i];
        const ts = searchResults.tses[i];
        const trackUri = searchResults.trackUris[i];

        rowData.push({
          id: eventId,
          ts: ts,
          trackUri: trackUri,
        });
      }
    }

    const description = searchResults
      ? `Search Results for "${searchText}" - ${searchResults.totalResults} results`
      : undefined;

    return m(
      DetailsShell,
      {
        title: 'Search Results',
        description,
        fillHeight: true,
      },
      m(DataGrid, {
        schema,
        rootSchema: 'data',
        initialColumns: [{field: 'id'}, {field: 'ts'}, {field: 'trackUri'}],
        data: rowData,
        fillHeight: true,
      }),
    );
  }
}
