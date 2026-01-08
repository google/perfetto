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
import {DataGrid} from '../../../components/widgets/datagrid/datagrid';
import {SchemaRegistry} from '../../../components/widgets/datagrid/datagrid_schema';
import {Row} from '../../../trace_processor/query_result';
import {SQLDataSource} from '../../../components/widgets/datagrid/sql_data_source';
import {SQLSchemaRegistry} from '../../../components/widgets/datagrid/sql_schema';
import {renderDocSection, renderWidgetShowcase} from '../widgets_page_utils';
import {App} from '../../../public/app';
import {Anchor} from '../../../widgets/anchor';

// Cache for the SQL data source - created once when page is first opened with a trace
let cachedSliceDataSource: SQLDataSource | undefined;

// SQL schema for slice table with track join
const SLICE_SQL_SCHEMA: SQLSchemaRegistry = {
  slice: {
    table: 'slice',
    columns: {
      id: {},
      ts: {},
      dur: {},
      track_id: {},
      track: {
        ref: 'track',
        foreignKey: 'track_id',
      },
      parent: {
        ref: 'slice',
        foreignKey: 'parent_id',
      },
      args: {
        expression: (alias, key) =>
          `extract_arg(${alias}.arg_set_id, '${key}')`,
        parameterized: true,
        parameterKeysQuery: (baseTable) => `
          SELECT DISTINCT args.key
          FROM ${baseTable}
          JOIN args ON args.arg_set_id = ${baseTable}.arg_set_id
          WHERE args.key IS NOT NULL
          ORDER BY args.key
          LIMIT 1000
        `,
      },
      all_args: {
        expression: (alias) =>
          `__intrinsic_arg_set_to_json(${alias}.arg_set_id)`,
      },
    },
  },
  track: {
    table: 'track',
    columns: {
      id: {},
      name: {},
    },
  },
};

// UI schema for slice table (defines how columns are displayed)
const SLICE_UI_SCHEMA: SchemaRegistry = {
  slice: {
    id: {
      title: 'ID',
      columnType: 'identifier',
    },
    ts: {
      title: 'Timestamp',
      columnType: 'quantitative',
    },
    dur: {
      title: 'Duration',
      columnType: 'quantitative',
    },
    name: {
      title: 'Name',
      columnType: 'text',
    },
    track_id: {
      title: 'Track ID',
      columnType: 'quantitative',
    },
    track: {
      ref: 'track',
      title: 'Track',
    },
    parent: {
      ref: 'slice',
      title: 'Parent',
    },
    args: {
      parameterized: true,
      title: 'Arg',
    },
    all_args: {
      title: 'All Args',
      columnType: 'text',
      cellRenderer: (value) => {
        if (value === null || value === undefined) {
          return m('span.pf-null-value', 'NULL');
        }
        try {
          const parsed = typeof value === 'string' ? JSON.parse(value) : value;
          if (typeof parsed !== 'object' || parsed === null) {
            return String(value);
          }
          const entries = Object.entries(parsed);
          if (entries.length === 0) {
            return m('span.pf-empty-value', '{}');
          }
          return m(
            'span.pf-args-list',
            entries.map(([key, val], i) => [
              i > 0 ? ', ' : '',
              m('b', key),
              ': ',
              String(val),
            ]),
          );
        } catch {
          return String(value);
        }
      },
    },
  },
  track: {
    id: {
      title: 'ID',
      columnType: 'quantitative',
    },
    name: {
      title: 'Name',
      columnType: 'text',
    },
  },
};

export function renderDataGrid(app: App): m.Children {
  // Create the SQL data source once when the page is first opened with a trace
  if (app.trace && !cachedSliceDataSource) {
    cachedSliceDataSource = new SQLDataSource({
      engine: app.trace.engine,
      sqlSchema: SLICE_SQL_SCHEMA,
      rootSchemaName: 'slice',
    });
  }

  return [
    m(
      '.pf-widget-intro',
      m('h1', 'DataGrid'),
      m('p', [
        'DataGrid is an opinionated data table and analysis tool designed for exploring ',
        'and analyzing SQL-like data with built-in sorting, filtering, and aggregation features. It is based on ',
        m(Anchor, {href: '#!/widgets/grid'}, 'Grid'),
        ' but unlike the grid component is specifically opinionated about the types of data it can receive.',
      ]),
      m('p', [
        'This example demonstrates a schema with multiple related tables: ',
        'employees, departments, and projects. It shows self-referential schemas ',
        '(manager -> employee), cross-references between tables, and parameterized ',
        'columns (skills).',
      ]),
      m('p', [
        'Try using the "Add column..." menu to explore nested relationships like ',
        '"manager.manager.name" or "department.head.name". For parameterized columns ',
        'like "skills", you can type any key name (e.g., "typescript", "python").',
      ]),
    ),

    renderWidgetShowcase({
      renderWidget: ({...rest}) => {
        return m(DataGrid, {
          ...rest,
          fillHeight: true,
          schema: EMPLOYEE_SCHEMA,
          rootSchema: 'employee',
          data: EMPLOYEE_DATA,
        });
      },
      initialOpts: {
        showExportButton: false,
        structuredQueryCompatMode: false,
        enablePivotControls: true,
      },
      noPadding: true,
    }),

    renderDocSection('Schema-Based Column Definition', [
      m(
        'p',
        'DataGrid uses a schema-based approach for column definitions. ' +
          'The schema defines the shape of available data, supporting nested ' +
          'relationships via named schema references.',
      ),
      m('p', 'Example schema structure:'),
      m(
        'pre',
        `const schema: SchemaRegistry = {
  slice: {
    id: { filterType: 'quantitative' },
    name: { title: 'Slice Name', filterType: 'text' },
    parent: { ref: 'slice' },  // Self-referential
    thread: { ref: 'thread' },
    args: { parameterized: true },  // Dynamic keys
  },
  thread: {
    name: { title: 'Thread Name' },
    process: { ref: 'process' },
  },
  process: {
    name: { title: 'Process Name' },
    pid: { filterType: 'quantitative' },
  },
};`,
      ),
    ]),

    renderDocSection('SQL Data Source with Schema', [
      m(
        'p',
        'SQLDataSource (with schema) generates optimized SQL queries with JOINs based on ' +
          'column paths. This example queries the slice table and can join to the ' +
          'track table via "track.name".',
      ),
      cachedSliceDataSource
        ? renderWidgetShowcase({
            renderWidget: ({...rest}) => {
              return m(DataGrid, {
                ...rest,
                fillHeight: true,
                schema: SLICE_UI_SCHEMA,
                rootSchema: 'slice',
                data: cachedSliceDataSource!,
                initialColumns: [
                  {field: 'id'},
                  {field: 'ts'},
                  {field: 'dur'},
                  {field: 'track.name'},
                ],
              });
            },
            initialOpts: {
              enableSortControls: true,
              enableFilterControls: true,
              enablePivotControls: false,
              showRowCount: true,
            },
            noPadding: true,
          })
        : m('.pf-empty-state', 'Load a trace to see the SQL DataGrid example'),
    ]),
  ];
}

// Complex multi-table schema demonstrating relationships
const EMPLOYEE_SCHEMA: SchemaRegistry = {
  employee: {
    id: {
      title: 'ID',
      columnType: 'quantitative',
    },
    name: {
      title: 'Name',
      columnType: 'text',
    },
    title: {
      title: 'Job Title',
      columnType: 'text',
    },
    email: {
      title: 'Email',
      columnType: 'text',
    },
    salary: {
      title: 'Salary',
      columnType: 'quantitative',
    },
    hireDate: {
      title: 'Hire Date',
      columnType: 'text',
    },
    // Self-referential: manager is also an employee
    manager: {
      ref: 'employee',
      title: 'Manager',
    },
    // Cross-reference to department
    department: {
      ref: 'department',
      title: 'Department',
    },
    // Cross-reference to current project
    project: {
      ref: 'project',
      title: 'Current Project',
    },
    // Parameterized column for dynamic skill ratings
    skills: {
      parameterized: true,
      title: 'Skills',
      columnType: 'quantitative',
    },
  },
  department: {
    id: {
      title: 'Dept ID',
      columnType: 'quantitative',
    },
    name: {
      title: 'Name',
      columnType: 'text',
    },
    budget: {
      title: 'Budget',
      columnType: 'quantitative',
    },
    location: {
      title: 'Location',
      columnType: 'text',
    },
    // Head of department is an employee
    head: {
      ref: 'employee',
      title: 'Department Head',
    },
  },
  project: {
    id: {
      title: 'Project ID',
      columnType: 'quantitative',
    },
    name: {
      title: 'Project Name',
      columnType: 'text',
    },
    status: {
      title: 'Status',
      columnType: 'text',
      distinctValues: true,
    },
    deadline: {
      title: 'Deadline',
      columnType: 'text',
    },
    // Project lead is an employee
    lead: {
      ref: 'employee',
      title: 'Project Lead',
    },
    // Project belongs to a department
    department: {
      ref: 'department',
      title: 'Owning Department',
    },
  },
};

// Sample data with flattened relationships using dot notation
const EMPLOYEE_DATA: Row[] = [
  {
    'id': 1,
    'name': 'Alice Chen',
    'title': 'CEO',
    'email': 'alice@example.com',
    'salary': 250000,
    'hireDate': '2015-01-15',
    'manager.id': null,
    'manager.name': null,
    'department.id': 1,
    'department.name': 'Executive',
    'department.budget': 5000000,
    'project.id': null,
    'project.name': null,
    'skills.leadership': 10,
    'skills.strategy': 9,
    'skills.communication': 9,
  },
  {
    'id': 2,
    'name': 'Bob Martinez',
    'title': 'VP Engineering',
    'email': 'bob@example.com',
    'salary': 180000,
    'hireDate': '2016-03-20',
    'manager.id': 1,
    'manager.name': 'Alice Chen',
    'manager.title': 'CEO',
    'department.id': 2,
    'department.name': 'Engineering',
    'department.budget': 2000000,
    'project.id': 1,
    'project.name': 'Platform Rewrite',
    'skills.leadership': 8,
    'skills.architecture': 9,
    'skills.python': 7,
    'skills.typescript': 8,
  },
  {
    'id': 3,
    'name': 'Carol Williams',
    'title': 'Senior Engineer',
    'email': 'carol@example.com',
    'salary': 150000,
    'hireDate': '2018-07-10',
    'manager.id': 2,
    'manager.name': 'Bob Martinez',
    'manager.title': 'VP Engineering',
    'manager.manager.name': 'Alice Chen',
    'department.id': 2,
    'department.name': 'Engineering',
    'department.budget': 2000000,
    'project.id': 1,
    'project.name': 'Platform Rewrite',
    'project.status': 'In Progress',
    'skills.typescript': 9,
    'skills.react': 8,
    'skills.sql': 7,
  },
  {
    'id': 4,
    'name': 'David Kim',
    'title': 'Engineer',
    'email': 'david@example.com',
    'salary': 120000,
    'hireDate': '2020-02-01',
    'manager.id': 3,
    'manager.name': 'Carol Williams',
    'manager.title': 'Senior Engineer',
    'manager.manager.name': 'Bob Martinez',
    'manager.manager.manager.name': 'Alice Chen',
    'department.id': 2,
    'department.name': 'Engineering',
    'project.id': 2,
    'project.name': 'Mobile App',
    'project.status': 'Planning',
    'skills.kotlin': 8,
    'skills.swift': 7,
    'skills.react': 6,
  },
  {
    'id': 5,
    'name': 'Eva Johnson',
    'title': 'VP Product',
    'email': 'eva@example.com',
    'salary': 175000,
    'hireDate': '2017-05-15',
    'manager.id': 1,
    'manager.name': 'Alice Chen',
    'department.id': 3,
    'department.name': 'Product',
    'department.budget': 1500000,
    'project.id': null,
    'project.name': null,
    'skills.leadership': 8,
    'skills.strategy': 8,
    'skills.ux': 7,
  },
  {
    'id': 6,
    'name': 'Frank Lee',
    'title': 'Product Manager',
    'email': 'frank@example.com',
    'salary': 130000,
    'hireDate': '2019-09-01',
    'manager.id': 5,
    'manager.name': 'Eva Johnson',
    'manager.title': 'VP Product',
    'manager.manager.name': 'Alice Chen',
    'department.id': 3,
    'department.name': 'Product',
    'project.id': 1,
    'project.name': 'Platform Rewrite',
    'project.status': 'In Progress',
    'skills.communication': 8,
    'skills.ux': 6,
    'skills.sql': 5,
  },
  {
    'id': 7,
    'name': 'Grace Park',
    'title': 'Engineer',
    'email': 'grace@example.com',
    'salary': 115000,
    'hireDate': '2021-01-10',
    'manager.id': 3,
    'manager.name': 'Carol Williams',
    'manager.manager.name': 'Bob Martinez',
    'department.id': 2,
    'department.name': 'Engineering',
    'project.id': 1,
    'project.name': 'Platform Rewrite',
    'project.status': 'In Progress',
    'skills.typescript': 7,
    'skills.python': 8,
    'skills.sql': 8,
  },
  {
    'id': 8,
    'name': 'Henry Wu',
    'title': 'Designer',
    'email': 'henry@example.com',
    'salary': 110000,
    'hireDate': '2020-06-15',
    'manager.id': 5,
    'manager.name': 'Eva Johnson',
    'department.id': 3,
    'department.name': 'Product',
    'project.id': 2,
    'project.name': 'Mobile App',
    'project.status': 'Planning',
    'skills.figma': 9,
    'skills.ux': 8,
    'skills.css': 7,
  },
];
