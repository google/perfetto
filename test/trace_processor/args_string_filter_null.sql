select string_value
from args
where string_value = NULL
UNION
select string_value
from args
where string_value != NULL
UNION
select string_value
from args
where string_value < NULL
UNION
select string_value
from args
where string_value <= NULL
UNION
select string_value
from args
where string_value > NULL
UNION
select string_value
from args
where string_value >= NULL
UNION
select string_value
from args
where string_value LIKE NULL
UNION
select string_value
from args
where string_value GLOB NULL
