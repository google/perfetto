SELECT
  tid,
  is_main_thread
FROM thread
WHERE tid IN (5, 7, 11, 12, 99)
ORDER BY tid;
