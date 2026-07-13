namespace dg {
  export function route(id: string, displayName?: string): any {}
  export function subroute(routes: any);
}

function setupRouter() {
  const slice: Record<string, any> = {
    id: dg.route('id'),
    ts: dg.route('ts', 'Timestamp'),
    track: dg.subroute({
      id: dg.route('id'),
      name: dg.route('name', 'Track Name'),
    }),
    arg: dg.param('arg', /^\S+$/),
  };

  return {
    ...slice,
    parent: slice, // Recursive slice
  };
}

// The router in the sql data source looks something like this:
// Don't have to define the special ones
const sqlSchema = {
  sql: 'select * from slice', // Expression or table name
  routes: {
    'track': {kind: 'relation', joinOn: (right) => `ON id = ${right}.track_id`},
    'arg.$1': {
      kind: 'expr',
      expr: (argName) => `extract_arg(${table}.arg_set_id, ${argName})`,
    },
  },
};

const sqlRouter = {
  use(paths) {
    //
  },
  // Lists all children in the current path.
  list(path: string[]) {},
};

/**
 * Valid routes:
 * - id
 * - ts
 * - track.name
 * - arg.foo
 */
