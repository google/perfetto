# Export to Dashboard

Makes the upstream data source available to dashboard tabs. Each export node
publishes its input columns so that dashboard charts can query the data.

## Configuration

- **Export name**: A human-readable label shown on dashboard charts and in the
  data panel. Defaults to the name of the connected input node.

## How It Works

1. Connect an upstream node (table, filter, join, etc.) to this node.
2. The node publishes the input's columns to the global dashboard registry.
3. Open a Dashboard tab and add charts that reference this exported source.

## Tips

- Give exports descriptive names — they appear as data source labels on
  dashboards.
- The table is materialized on first use; dashboards trigger execution
  automatically if the source hasn't been run yet.
