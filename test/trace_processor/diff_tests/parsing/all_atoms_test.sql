SELECT slice.name, args.key, args.display_value FROM track JOIN slice ON track.id = slice.track_id JOIN args USING(arg_set_id) WHERE track.name = "Statsd Atoms";
