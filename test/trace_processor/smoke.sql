SELECT * from sched limit 10;

SELECT * from counters limit 10;

CREATE VIRTUAL TABLE sp USING span(sched, window, cpu);

SELECT * from sp order by ts limit 10;
