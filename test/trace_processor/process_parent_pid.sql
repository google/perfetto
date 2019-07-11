SELECT
  child.pid as child_pid,
  parent.pid as parent_pid
FROM process as child
INNER JOIN process as parent
ON child.parent_upid = parent.upid
ORDER BY child_pid
