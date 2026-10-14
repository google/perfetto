# Local Bigtrace

`tools/bigtrace_server.py` runs PerfettoSQL on many local traces at once and
collects the rows in DuckDB tables, for the Bigtrace UI and the command line:

```bash
tools/bigtrace_server.py serve ~/traces --generate-manifest   # terminal 1
tools/bigtrace_server.py query --table slices "SELECT name, dur FROM slice"
tools/bigtrace_server.py sql "SELECT name, count(*) FROM slices GROUP BY 1 ORDER BY 2 DESC"
```

- Guide: [docs/analysis/bigtrace-local.md](/docs/analysis/bigtrace-local.md)
- Design: [docs/design-docs/bigtrace-local.md](/docs/design-docs/bigtrace-local.md)
- Options: `tools/bigtrace_server.py <serve|query|sql> --help`

`bigtrace_manifest.json` and `bigtrace_manifest.csv` here are example
manifests for the traces in `test/data`
(`tools/bigtrace_server.py serve test/data -m examples/bigtrace/bigtrace_manifest.json`).
