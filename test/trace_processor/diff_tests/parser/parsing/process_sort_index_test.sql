-- Verify that legacy_sort_index from ProcessDescriptor is parsed and stored as process_sort_index_hint

-- Check that processes have the correct sort index hints
SELECT
  pid,
  name,
  extract_arg(arg_set_id, 'process_sort_index_hint') AS sort_index_hint
FROM process
ORDER BY pid;
