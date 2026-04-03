#!/bin/bash
# Tests that .pfgraph files produce identical output to their .sql counterparts.
# Run from the perfetto root directory after building:
#   tools/test_pfgraph.sh out/lnx
#
# This script:
# 1. Finds all .pfgraph files alongside .sql files in the stdlib
# 2. For each pair, swaps the .pfgraph content into the .sql file
# 3. Rebuilds trace_processor_shell
# 4. Runs the relevant diff tests
# 5. Restores the original .sql file
# 6. Reports pass/fail

set -e

OUT_DIR="${1:-out/lnx}"
STDLIB_DIR="src/trace_processor/perfetto_sql/stdlib"
PASS=0
FAIL=0
ERRORS=""

if [ ! -f "$OUT_DIR/trace_processor_shell" ]; then
  echo "ERROR: $OUT_DIR/trace_processor_shell not found. Build first."
  exit 1
fi

# Find all .pfgraph files with corresponding .sql files
PFGRAPH_FILES=$(find "$STDLIB_DIR" -name "*.pfgraph" | sort)

if [ -z "$PFGRAPH_FILES" ]; then
  echo "No .pfgraph files found in $STDLIB_DIR"
  exit 0
fi

echo "Found $(echo "$PFGRAPH_FILES" | wc -l) .pfgraph files to test"
echo "========================================"

for PFGRAPH in $PFGRAPH_FILES; do
  SQL="${PFGRAPH%.pfgraph}.sql"
  if [ ! -f "$SQL" ]; then
    echo "SKIP: $PFGRAPH (no matching .sql file)"
    continue
  fi

  # Derive the module name for the diff test filter
  REL_PATH="${PFGRAPH#$STDLIB_DIR/}"
  MODULE_DIR=$(dirname "$REL_PATH")
  MODULE_BASE=$(basename "$REL_PATH" .pfgraph)
  # Build a test filter: match tests that likely use this module
  TEST_FILTER=".*${MODULE_BASE}.*"

  echo ""
  echo "Testing: $PFGRAPH"
  echo "  against: $SQL"
  echo "  filter: $TEST_FILTER"

  # Backup and swap
  cp "$SQL" "${SQL}.bak"
  cp "$PFGRAPH" "$SQL"

  # Rebuild
  if ! tools/ninja -C "$OUT_DIR" trace_processor_shell 2>&1 | tail -1; then
    echo "  BUILD FAILED"
    cp "${SQL}.bak" "$SQL"
    rm "${SQL}.bak"
    FAIL=$((FAIL + 1))
    ERRORS="$ERRORS\n  FAIL (build): $PFGRAPH"
    continue
  fi

  # Run diff tests
  RESULT=$(tools/diff_test_trace_processor.py "$OUT_DIR/trace_processor_shell" \
    --keep-input --quiet --name-filter="$TEST_FILTER" 2>&1 | tail -3)

  PASSED=$(echo "$RESULT" | grep "PASSED" | grep -o "[0-9]*" | head -1)
  FAILED=$(echo "$RESULT" | grep "FAILED" | grep -o "[0-9]*" | tail -1)

  if [ -z "$FAILED" ] || [ "$FAILED" = "0" ]; then
    echo "  PASSED ($PASSED tests)"
    PASS=$((PASS + 1))
  else
    echo "  FAILED ($FAILED tests failed, $PASSED passed)"
    FAIL=$((FAIL + 1))
    ERRORS="$ERRORS\n  FAIL: $PFGRAPH ($FAILED tests)"
  fi

  # Restore
  cp "${SQL}.bak" "$SQL"
  rm "${SQL}.bak"
done

# Rebuild with original files
tools/ninja -C "$OUT_DIR" trace_processor_shell >/dev/null 2>&1

echo ""
echo "========================================"
echo "Results: $PASS passed, $FAIL failed"
if [ -n "$ERRORS" ]; then
  echo -e "\nFailures:$ERRORS"
fi
exit $FAIL
