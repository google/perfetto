-- Verify that legacy_sort_index from ThreadDescriptor is parsed and stored as thread_sort_index_hint

-- Check that threads have the correct sort index hints
SELECT
  tid,
  name,
  extract_arg(arg_set_id, 'thread_sort_index_hint') AS sort_index_hint
FROM thread
ORDER BY tid;
