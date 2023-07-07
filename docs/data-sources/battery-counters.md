# Power data sources

On Android Perfetto bundles data sources to retrieve power
counters from the device power management units (where supported).

## Battery counters

_This data source has been introduced in Android 10 (Q) and requires the
presence of power-management hardware on the device. This is available on 
most Google Pixel smartphones._

Modern smartphones are equipped with a power monitoring IC which is able to
measure the charge flowing in and out of the battery. This allows Perfetto to
observe the total and instantaneous charge drained from the battery by the
overall device (the union of SoC, display, radios and all other hardware
units).

A simplified block diagram:

![](/docs/images/battery-counters.png "Schematic diagram of battery counters")

These counters report:

* The remaining battery capacity in %.
* The remaining battery charge in microampere-hours (µAh).
* The instantaneous (typically the average over a small window of time) current
  in microampere (µA)

The presence and the resolution of these counters depends on the device
manufacturer. At the platform level this data is obtained polling the
Android [IHealth HAL][health-hal].
For more details on HW specs and resolution see
[Measuring Device Power](https://source.android.com/devices/tech/power/device).

[health-hal]: https://cs.android.com/android/platform/superproject/+/main:hardware/interfaces/health/2.0/IHealth.hal?q=IHealth

#### Measuring charge while plugged on USB

Battery counters measure the charge flowing *in* and *out* of
the battery. If the device is plugged to a USB cable, you will likely observe
a negative instantaneous current and an increase of the total charge, denoting
the fact that charge is flowing in the battery (i.e. charging it) rather
than out.

This can make measurements in lab settings problematic. The known workarounds
for this are:

* Using specialized USB hubs that allow to electrically disconnect the USB ports
  from the host side. This allows to effectively disconnect the phone while the
  tests are running.

* On rooted phones the power management IC driver allows to disconnect the USB
  charging while keeping the USB data link active. This feature is
  SoC-specific, is undocumented and not exposed through any HAL.
  For instance on a Pixel 2 this can be achieved running, as root:
  `echo 1 > /sys/devices/soc/800f000.qcom,spmi/spmi-0/spmi0-02/800f000.qcom,spmi:qcom,pmi8998@2:qcom,qpnp-smb2/power_supply/battery/input_suspend`.
  Note that in most devices the kernel USB driver holds a wakelock to keep the
  USB data link active, so the device will never fully suspend even when turning
  the screen off.

### UI

![](/docs/images/battery-counters-ui.png)

### SQL

```sql
select ts, t.name, value from counter as c left join counter_track t on c.track_id = t.id
```

ts | name | value
---|------|------
338297039804951 | batt.charge_uah | 2085000
338297039804951 | batt.capacity_pct | 75
338297039804951 | batt.current_ua | -1469687
338297145212097 | batt.charge_uah | 2085000
338297145212097 | batt.capacity_pct | 75
338297145212097 | batt.current_ua | -1434062

### TraceConfig

Trace proto:
[BatteryCounters](/docs/reference/trace-packet-proto.autogen#BatteryCounters)

Config proto:
[AndroidPowerConfig](/docs/reference/trace-config-proto.autogen#AndroidPowerConfig)

Sample config (Android):

```protobuf
data_sources: {
    config {
        name: "android.power"
        android_power_config {
            battery_poll_ms: 250
            battery_counters: BATTERY_COUNTER_CAPACITY_PERCENT
            battery_counters: BATTERY_COUNTER_CHARGE
            battery_counters: BATTERY_COUNTER_CURRENT
        }
    }
}
```

Sample Config (Chrome OS or Linux):

```protobuf
data_sources: {
    config {
        name: "linux.sysfs_power"
    }
}
```

## {#odpm} On-Device Power Rails Monitor (ODPM)

_This data source has been introduced in Android 10 (Q) and requires the
dedicated hardware on the device. This hardware is not yet available on
most production phones._

Recent version of Android introduced the support for more advanced power
monitoring at the hardware subsystem level, known as
"On-Device Power Rail Monitors" (ODPMs).
These counters measure the energy drained by (groups of) hardware units.

Unlike the battery counters, they are not affected by the charging/discharging
state of the battery, because they measure power downstream of the battery.

The presence and the resolution of power rail counters depends on the device
manufacturer. At the platform level this data is obtained polling the
Android [IPowerStats HAL][power-hal].

Googlers: See [go/power-rails-internal-doc](http://go/power-rails-internal-doc)
for instructions on how to change the refault rail selection on Pixel devices.

[power-hal]: https://cs.android.com/android/platform/superproject/+/main:hardware/interfaces/power/stats/1.0/IPowerStats.hal

Simplified block diagram:

![](/docs/images/power-rails.png "Block diagram of ODPMs")

### TraceConfig

Trace proto:
[PowerRails](/docs/reference/trace-packet-proto.autogen#PowerRails)

Config proto:
[AndroidPowerConfig](/docs/reference/trace-config-proto.autogen#AndroidPowerConfig)

Sample config:

```protobuf
data_sources: {
    config {
        name: "android.power"
        android_power_config {
            battery_poll_ms: 250
            collect_power_rails: true
            # Note: it is possible to specify both rails and battery counters
            # in this section.
        }
    }
}
```

## Related data sources

See also the [CPU -> Frequency scaling](cpu-freq.md) data source.
