# Checks

Every health check CANary ships with, generated from the vehicle profiles by
`tools/gen_checks_doc.py`. Limits are in the units the profiles use (metric); the web page
and Glovebox show them in the units you chose.

A check counts only while its conditions hold, and gives a verdict once they have held for
long enough in total. Before that it is listed as waiting. If the car does not report a
reading the check needs, it is listed as not available.

The limits are starting points drawn from service literature and owner experience, set so
that a healthy engine passes with room to spare. They are not manufacturer test
specifications. If your car proves one of them wrong, please
[open an issue](https://github.com/au12/CANary/issues).

| Check | Reports | Runs on | Worst level |
| --- | --- | --- | --- |
| [Boost pressure](#boost-pressure) | Boost below target | Any car | Critical |
| [Boost overshoot](#boost-overshoot) | Boost above target | Any car | Critical |
| [Intake air temperature](#intake-air-temperature) | Intake air much hotter than outside | Any car | Warning |
| [Fuel mixture at idle](#fuel-mixture-at-idle) | Running lean at idle / Running rich at idle | Any car | Warning |
| [Fuel mixture under load](#fuel-mixture-under-load) | Running lean under load / Running rich under load | Any car | Critical |
| [Fuel mixture, bank to bank](#fuel-mixture-bank-to-bank) | Cylinder banks disagree on fuel | Any car | Warning |
| [High-pressure fuel](#high-pressure-fuel) | Fuel rail pressure below target | Volkswagen EA888 Gen 3 | Critical |
| [Low-pressure fuel](#low-pressure-fuel) | Low-side fuel pressure low | Volkswagen EA888 Gen 3 | Critical |
| [Knock control](#knock-control) | Engine pulling ignition timing | Volkswagen EA888 Gen 3 | Critical |
| [Knock, cylinder to cylinder](#knock-cylinder-to-cylinder) | One cylinder knocks more than the others | Volkswagen EA888 Gen 3 | Warning |
| [Misfires](#misfires) | Misfires counted | Volkswagen EA888 Gen 3 | Critical |
| [Intake cam adaptation](#intake-cam-adaptation) | Intake cam timing has drifted | Volkswagen EA888 Gen 3 | Warning |
| [Exhaust cam adaptation](#exhaust-cam-adaptation) | Exhaust cam timing has drifted | Volkswagen EA888 Gen 3 | Warning |
| [Variable valve timing](#variable-valve-timing) | Cam timing not following its target | Volkswagen EA888 Gen 3 | Warning |
| [Coolant temperature](#coolant-temperature) | Engine running hot | Any car | Critical |
| [Warm-up time](#warm-up-time) | Slow to warm up | Any car | Warning |
| [Oil temperature](#oil-temperature) | Oil running hot | Any car | Critical |
| [Charging voltage](#charging-voltage) | Charging voltage low | Any car | Critical |
| [Over-voltage](#over-voltage) | Charging voltage too high | Any car | Critical |
| [Catalyst temperature](#catalyst-temperature) | Catalyst running hot | Any car | Critical |
| [Self-test history](#self-test-history) | Trouble codes were cleared recently | Any car | Advisory |

## Boost

### Boost pressure

Reports **Boost below target**.

| | |
| --- | --- |
| Counts while | hard acceleration above 2000 rpm: `accel_pedal > 60 && rpm > 2000 && boost_target > baro + 35` |
| Measures | the 90th percentile of `boost_target - boost_actual` |
| Verdict | skips the first 2 seconds each time the conditions begin; needs 6 seconds in total |
| Advisory, warning, critical | above 12 kPa, above 20 kPa, above 35 kPa |
| Profile | `generic-obd2`, id `boost_under_target` |

The turbo is not delivering the pressure requested. This often shows up here before a P0299 code or limp mode.

Common causes: boost leak (charge pipes, couplers, intercooler); diverter (recirculation) valve leaking; wastegate actuator or its control; PCV valve failure; worn turbocharger.

### Boost overshoot

Reports **Boost above target**.

| | |
| --- | --- |
| Counts while | hard acceleration above 2000 rpm: `accel_pedal > 60 && rpm > 2000 && boost_target > baro + 35` |
| Measures | the 90th percentile of `max(boost_actual - boost_target, 0)` |
| Verdict | skips the first 2 seconds each time the conditions begin; needs 6 seconds in total |
| Advisory, warning, critical | none, above 30 kPa, above 45 kPa |
| Profile | `generic-obd2`, id `boost_over_target` |

More boost than requested stresses the engine and can trigger an overboost code (P0234).

Common causes: wastegate sticking closed; boost control solenoid or its vacuum lines; boost pressure sensor.

### Intake air temperature

Reports **Intake air much hotter than outside**.

| | |
| --- | --- |
| Counts while | driving at road speed: `speed > 50 && rpm > 1000` |
| Measures | the 90th percentile of `intake_air_temp - ambient_temp` |
| Verdict | needs 2 minutes in total |
| Advisory, warning, critical | above 32 °C, above 45 °C, none |
| Profile | `generic-obd2`, id `charge_temp_rise` |

Hot intake air costs power and makes the engine pull ignition timing.

Common causes: intercooler blocked or its airflow obstructed; heat soak after stop-and-go driving; intake temperature sensor fault.

## Fuel mixture

### Fuel mixture at idle

Reports **Running lean at idle** on the high side and **Running rich at idle** on the low side.

| | |
| --- | --- |
| Counts while | idling with a warm engine: `rpm > 500 && rpm < 1100 && speed < 3 && coolant_temp > 70` |
| Measures | the average of `fuel_trim_b1` |
| Verdict | skips the first 3 seconds each time the conditions begin; needs 30 seconds in total |
| Advisory, warning, critical | above 9%, above 15%, none |
| Low side | below −9%, below −15%, none |
| Profile | `generic-obd2`, id `fuel_trim_idle` |

Extra fuel at idle that fades as load rises points to air getting in after the air meter. Left alone it leads to rough idle and lean codes (P0171).

Common causes: vacuum or intake leak (hoses, PCV valve, intake manifold gasket); leaking brake booster line; dirty or failing air mass / pressure sensor; exhaust leak ahead of the oxygen sensor.

On the low side: The engine is getting more fuel than it asks for at idle.

Common causes: leaking injector; purge valve stuck open; fuel in the oil from many short trips; air sensor reading high.

### Fuel mixture under load

Reports **Running lean under load** on the high side and **Running rich under load** on the low side.

| | |
| --- | --- |
| Counts while | accelerating with a warm engine: `engine_load > 60 && rpm > 2000 && coolant_temp > 70` |
| Measures | the average of `fuel_trim_b1` |
| Verdict | skips the first 1 seconds each time the conditions begin; needs 12 seconds in total |
| Advisory, warning, critical | above 9%, above 14%, above 22% |
| Low side | below −10%, below −15%, none |
| Profile | `generic-obd2`, id `fuel_trim_load` |

Needing extra fuel when working hard means fuel delivery is not keeping up, or the engine is under-reading its air. Lean under load is the dangerous direction for a turbo engine.

Common causes: weak fuel pump or clogged filter; high-pressure pump or its cam follower wearing; air mass sensor under-reading; boost leak after the air meter.

On the low side: The engine is getting more fuel than it asks for when working hard.

Common causes: leaking or over-delivering injector; fuel pressure too high; boost leak on engines that measure air by flow; air sensor reading high.

### Fuel mixture, bank to bank

Reports **Cylinder banks disagree on fuel**.

| | |
| --- | --- |
| Counts while | driving with a warm engine: `coolant_temp > 70 && rpm > 500` |
| Measures | the average of `abs(fuel_trim_b1 - fuel_trim_b2)` |
| Verdict | needs 2 minutes in total |
| Advisory, warning, critical | above 7%, above 12%, none |
| Profile | `generic-obd2`, id `fuel_trim_bank_split` |

A fault on one side only: one bank is compensating for something the other is not.

Common causes: intake leak on one bank; injector or oxygen sensor on one bank; exhaust leak on one bank.

## Fuel delivery

### High-pressure fuel

Reports **Fuel rail pressure below target**.

| | |
| --- | --- |
| Counts while | accelerating above 1500 rpm: `rpm > 1500 && engine_load > 40 && rail_pressure_target > 3000` |
| Measures | the 90th percentile of `(rail_pressure_target - rail_pressure) / rail_pressure_target * 100` |
| Verdict | skips the first 1.5 seconds each time the conditions begin; needs 10 seconds in total |
| Advisory, warning, critical | above 9%, above 15%, above 25% |
| Profile | `vw-ea888-gen3`, id `rail_pressure_low` |

The high-pressure fuel system is not keeping up with demand.

Common causes: high-pressure fuel pump; low-side pump or filter not feeding it; fuel pressure regulator valve; leaking injector.

### Low-pressure fuel

Reports **Low-side fuel pressure low**.

| | |
| --- | --- |
| Counts while | the engine running: `rpm > 600` |
| Measures | the lowest value of `lp_fuel_pressure` |
| Verdict | needs 60 seconds in total |
| Advisory, warning, critical | none, below 330 kPa, below 250 kPa |
| Profile | `vw-ea888-gen3`, id `lp_fuel_low` |

The in-tank pump feeds the high-pressure pump. If it sags, the engine leans out under load.

Common causes: in-tank fuel pump; fuel pump control module; clogged fuel filter.

## Ignition

### Knock control

Reports **Engine pulling ignition timing**.

| | |
| --- | --- |
| Counts while | firm acceleration with a warm engine: `accel_pedal > 45 && rpm > 1800 && coolant_temp > 70` |
| Measures | the 95th percentile of `knock_retard_worst` |
| Verdict | skips the first 0.5 seconds each time the conditions begin; needs 6 seconds in total |
| Advisory, warning, critical | above 3.5°, above 5°, above 8° |
| Profile | `vw-ea888-gen3`, id `knock_retard` |

A little knock retard is normal. Consistently large corrections mean the engine is protecting itself and giving up power.

Common causes: low-octane or poor fuel; carbon build-up on intake valves and pistons; hot intake air (see intake air temperature); worn spark plugs or wrong heat range; oil dilution or low oil quality.

### Knock, cylinder to cylinder

Reports **One cylinder knocks more than the others**.

| | |
| --- | --- |
| Counts while | firm acceleration with a warm engine: `accel_pedal > 45 && rpm > 1800 && coolant_temp > 70` |
| Measures | the 90th percentile of `knock_retard_spread` |
| Verdict | skips the first 0.5 seconds each time the conditions begin; needs 6 seconds in total |
| Advisory, warning, critical | above 3°, above 4.5°, none |
| Profile | `vw-ea888-gen3`, id `knock_one_cylinder` |

When a single cylinder stands out, the cause is local to it.

Common causes: spark plug or ignition coil on that cylinder; injector spray pattern or flow; carbon deposits in that cylinder.

### Misfires

Reports **Misfires counted**.

| | |
| --- | --- |
| Counts while | the engine running: `rpm > 500` |
| Measures | the highest value of `misfire_total` |
| Verdict | needs 60 seconds in total |
| Advisory, warning, critical | above 2, above 12, above 60 |
| Profile | `vw-ea888-gen3`, id `misfires` |

Misfires send unburned fuel into the catalyst. A flashing check-engine light means stop driving hard now.

Common causes: ignition coil or spark plug; injector; intake leak; carbon build-up on intake valves; low compression.

## Valve timing

### Intake cam adaptation

Reports **Intake cam timing has drifted**.

| | |
| --- | --- |
| Counts while | the engine running: `rpm > 500` |
| Measures | the latest value of `abs(cam_adapt_in)` |
| Verdict | needs 5 seconds in total |
| Advisory, warning, critical | above 3°, above 5°, none |
| Profile | `vw-ea888-gen3`, id `cam_adaptation_intake` |

The engine computer learns how far the intake camshaft sits from where the crankshaft says it should be. On the earlier EA888 engines (Gen 1 and 2), an offset beyond about 5° is the accepted sign of a stretched timing chain. This generation reports the same figure, but it is not established that it follows chain wear the same way, and owners often see it stay near zero at high mileage. A figure that grows deserves attention. A small one is not proof of a healthy chain: a rattle on cold starts and the tensioner's inspection port are what settle that.

Common causes: timing chain elongation; chain tensioner; cam adjuster or its control valve; A camshaft or crankshaft sensor reading.

### Exhaust cam adaptation

Reports **Exhaust cam timing has drifted**.

| | |
| --- | --- |
| Counts while | the engine running: `rpm > 500` |
| Measures | the latest value of `abs(cam_adapt_ex)` |
| Verdict | needs 5 seconds in total |
| Advisory, warning, critical | above 3.5°, above 5°, none |
| Profile | `vw-ea888-gen3`, id `cam_adaptation_exhaust` |

The same learned offset for the exhaust camshaft. See the intake side for what it does and does not show.

Common causes: timing chain elongation; chain tensioner; cam adjuster or its control valve.

### Variable valve timing

Reports **Cam timing not following its target**.

| | |
| --- | --- |
| Counts while | driving above 1500 rpm with a warm engine: `rpm > 1500 && coolant_temp > 70` |
| Measures | the 95th percentile of `abs(cam_in_actual - cam_in_target)` |
| Verdict | needs 60 seconds in total |
| Advisory, warning, critical | above 5°, above 8°, none |
| Profile | `vw-ea888-gen3`, id `cam_tracking` |

The variable valve timing adjuster is slow or not reaching position.

Common causes: cam adjuster solenoid (magnet); low or dirty oil; cam adjuster itself.

## Cooling

### Coolant temperature

Reports **Engine running hot**.

| | |
| --- | --- |
| Counts while | the engine running: `rpm > 400` |
| Measures | the highest value of `coolant_temp` |
| Verdict | needs 60 seconds in total |
| Advisory, warning, critical | none, above 113 °C, above 120 °C |
| Profile | `generic-obd2`, id `coolant_overheat` |

Sustained temperatures this high risk head-gasket and turbo damage. Stop and let the engine cool if the gauge or a warning confirms it.

Common causes: low coolant level or a leak; failing water pump or thermostat housing; cooling fan not running; blocked radiator.

### Warm-up time

Reports **Slow to warm up**.

| | |
| --- | --- |
| Counts while | the engine running from cold: `rpm > 400` |
| Measures | the time until `coolant_temp >= 75` holds |
| Verdict | needs 10 minutes in total |
| Advisory, warning, critical | above 18 minutes, above 25 minutes, none |
| Profile | `generic-obd2`, id `warmup_slow` |

An engine that stays cold burns more fuel, wears faster and can hold back boost. Short winter trips can trigger this without a fault.

Common causes: thermostat stuck open; coolant temperature sensor reading low; very short trips in cold weather.

## Lubrication

### Oil temperature

Reports **Oil running hot**.

| | |
| --- | --- |
| Counts while | the engine running: `rpm > 400` |
| Measures | the highest value of `oil_temp` |
| Verdict | needs 60 seconds in total |
| Advisory, warning, critical | none, above 130 °C, above 140 °C |
| Profile | `generic-obd2`, id `oil_overheat` |

Oil this hot thins out and breaks down quickly.

Common causes: sustained high load or towing; low oil level; oil cooler or its coolant supply.

## Electrical

### Charging voltage

Reports **Charging voltage low**.

| | |
| --- | --- |
| Counts while | the engine above idle speed: `rpm > 1000` |
| Measures | the 90th percentile of `vbat` |
| Verdict | needs 2 minutes in total |
| Advisory, warning, critical | none, below 13.1 V, below 12.6 V |
| Profile | `generic-obd2`, id `charging_low` |

The alternator is not lifting system voltage. Smart charging can hold voltage low for a while, so this looks at the upper end of what was seen.

Common causes: alternator or its regulator; drive belt slipping; battery at end of life; poor ground or battery terminal connection.

### Over-voltage

Reports **Charging voltage too high**.

| | |
| --- | --- |
| Counts while | the engine above idle speed: `rpm > 1000` |
| Measures | the 95th percentile of `vbat` |
| Verdict | needs 2 minutes in total |
| Advisory, warning, critical | none, above 15.4 V, above 16 V |
| Profile | `generic-obd2`, id `charging_high` |

Over-voltage cooks the battery and can damage electronics. Cars that recover energy while slowing down briefly charge near 15 V on purpose, so the limit sits above that.

Common causes: voltage regulator fault; battery sensor fault.

## Exhaust

### Catalyst temperature

Reports **Catalyst running hot**.

| | |
| --- | --- |
| Counts while | the engine running: `rpm > 400` |
| Measures | the highest value of `cat_temp` |
| Verdict | needs 60 seconds in total |
| Advisory, warning, critical | none, above 920 °C, above 980 °C |
| Profile | `generic-obd2`, id `catalyst_overheat` |

Very high catalyst temperature usually means unburned fuel is reaching it.

Common causes: misfires; rich mixture; sustained full-load driving.

## Diagnostics

### Self-test history

Reports **Trouble codes were cleared recently**.

| | |
| --- | --- |
| Counts while | the engine running: `rpm > 400` |
| Measures | the latest value of `clr_dist` |
| Verdict | needs 5 seconds in total |
| Advisory, warning, critical | below 80 km, none, none |
| Profile | `generic-obd2`, id `codes_cleared_recently` |

After a clear (or a battery disconnect) the self-tests start over. A fault can be present without a code until they have run again.

Common causes: battery was disconnected or replaced; codes were erased with a scan tool.
