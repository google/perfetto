# Android Game Intervention List

_This data source is supported only on Android userdebug builds._

The "android.game_interventions" data source gathers the available game modes
and game interventions for each game.

Use this data to compare or document traces of the same game under different
game modes or interventions.

### UI

The trace info page shows game interventions in a table.

![](/docs/images/android_game_interventions.png "Android game intervention list in the UI")

### SQL

Game intervention data is stored in the following SQL table:

* [`android_game_intervention_list`](/docs/analysis/sql-tables.autogen#android_game_intervention_list)

This query shows each game's supported modes (with interventions) and current
game mode.

```sql
select package_name, current_mode, standard_mode_supported, perf_mode_supported, battery_mode_supported
from android_game_intervention_list
order by package_name
```
package_name | current_mode | standard_mode_supported | perf_mode_supported | battery_mode_supported
-------------|--------------|-------------------------|---------------------------|-----------------------
com.supercell.clashofclans | 1 | 1 | 0 | 1
com.mobile.legends | 3 | 1 | 0 | 1
com.riot.league.wildrift | 1 | 1 | 0 | 1

### TraceConfig

Android game intervention list is configured through [AndroidGameInterventionListConfig](/docs/reference/trace-config-proto.autogen#AndroidGameInterventionListConfig) section of trace config.

Sample config:

```protobuf
data_sources: {
    config {
        name: "android.game_interventions"
        android_game_intervention_list_config {
            package_name_filter: "com.my.game1"
            package_name_filter: "com.my.game2"
        }
    }
}
```
