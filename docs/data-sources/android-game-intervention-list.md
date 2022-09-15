# Android Game Intervention List

_This data source is supported only on Android userdebug builds._

The "android.game_interventions" data source gathers  the list of available game modes and game interventions of each game.

This allows you to better compare between or document traces of the same game but under different game mode or with different game intervention.

### UI

At the UI level, game interventions are shown as a table in trace info page.

![](/docs/images/android_game_interventions.png "Android game intervention list in the UI")

### SQL

At the SQL level, game interventions data is written in the following table:

* [`android_game_intervention_list`](docs/analysis/sql-tables.autogen#android_game_intervention_list)

Below is an example of querying what modes are supported (with interventions) and the current game mode of each game.

```sql
select package_name, current_mode, standard_mode_supported, performance_mode_supported, battery_mode_supported
from android_game_intervention_list
order by package_name
```
package_name | current_mode | standard_mode_supported | performance_mode_supported | battery_mode_supported
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
