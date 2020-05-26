SELECT ts, name, cpu, key, int_value, string_value FROM raw JOIN args ON raw.arg_set_id == args.arg_set_id WHERE name == "sched_waking" ORDER BY cpu ASC, ts ASC;
