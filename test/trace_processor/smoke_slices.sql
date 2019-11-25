select track.type as type, depth, count(*) as count
from slice
inner join track on slice.track_id = track.id
group by track.type, depth
order by track.type, depth;
