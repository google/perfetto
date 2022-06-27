SELECT
  freq,
  GROUP_CONCAT(cpu_id) AS cpus
FROM cpu_freq
GROUP BY freq
ORDER BY freq;
