SELECT
    ts,
    value,
    REPLACE(name, " Freq Limit", "") AS cpu
FROM
    counter AS c
LEFT JOIN
    counter_track AS t
    ON c.track_id = t.id
WHERE
    name GLOB "* Freq Limit"
ORDER BY ts;
