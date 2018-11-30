select ts,
       dur,
       counters.name as counters_name,
       value,
       ref,
       ref_type,
       id,
       args.key as args_key,
       int_value as utid
from counters
inner join args using(id)
where ref = 1
limit 10;
