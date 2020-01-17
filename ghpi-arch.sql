-- Sqlite schema for greenhouse monitoring system.
-- NOTE: when installing a new system, modify the Config table according to the calibration certificate for the PHFS_01e sensor. Also record electricity cost, gas cost, furnace burn rate, and furnace ignition delay.

CREATE TABLE Config(var TEXT PRIMARY KEY COLLATE NOCASE, val BLOB NOT NULL) WITHOUT ROWID;
INSERT INTO Config VALUES ('PHFS_01e_serial_number', 11053);
INSERT INTO Config VALUES ('PHFS_01e_S_calib', 1.21); -- Sensitivity calibration data for thermal flux sensor, in μV/(W/m²)
INSERT INTO Config VALUES ('electricity_cost', 0.15); -- $/kWh
INSERT INTO Config VALUES ('gas_cost', 0.59); -- $/therm
INSERT INTO Config VALUES ('furnace_burn_rate', 130000); -- BTU/hr
INSERT INTO Config VALUES ('furnace_ignition_delay', 30); -- seconds
INSERT INTO Config VALUES ('elevation', 1482); -- meters
INSERT INTO Config VALUES ('flux_area', 400); -- m² (cross section, parallel to sensor)
INSERT INTO Config VALUES ('exch_flow_rate', 757); -- mL/s
INSERT INTO Config VALUES ('csp_flow_rate', 0); -- mL/s

CREATE TABLE Idle_heartbeats(sensor_name TEXT PRIMARY KEY, sec INT NOT NULL) WITHOUT ROWID; -- Daemons update this timestamp for a sensor if the last recorded reading (or set of readings, for compound sensor units) equals the new reading and both the timestamp of the last recorded reading and the timestamp of the last idle heartbeat are more than five seconds old. Thus, an idle heartbeat shows when a sensor is live but unchanged. When the reading does change, it's recorded, and that itself shows that the sensor is live, so an idle heartbeat would be superfluous.
-- When a daemon for a sensor starts, and the sensor's last record is not null, one cs is added to its timestamp or the idle heartbeat (whichever is later) to calculate the timestamp of a null record to insert to show the absense of data from then until the next recorded reading (since otherwise that recordless period would mean the reading was known and unchanged).
-- When a sensor disappears or gives an invalid reading (invalid range or CRC), and the last record isn't null, a null is immediately recorded. Either way, the idle heartbeat isn't updated.
-- If the last record is not null, and both it and the idle heartbeat are more than five seconds old, then the daemon is dead.
-- For DS18B20, Idle_heartbeats.sensor_name is DS18B20_IDs.serial_code. For all other sensors, chip model number is the name, since there's only one of each.
INSERT INTO Idle_heartbeats VALUES ('MAX11201B', 0);
INSERT INTO Idle_heartbeats VALUES ('Furnace', 0);
INSERT INTO Idle_heartbeats VALUES ('BME680', 0);
INSERT INTO Idle_heartbeats VALUES ('SHT31', 0);
INSERT INTO Idle_heartbeats VALUES ('TSL2591', 0);
INSERT INTO Idle_heartbeats VALUES ('VEML6075', 0);
INSERT INTO Idle_heartbeats VALUES ('INA260', 0);

CREATE TABLE DS18B20_IDs(sensor_ID INTEGER PRIMARY KEY NOT NULL, serial_code TEXT UNIQUE NOT NULL, label TEXT UNIQUE NOT NULL, CHECK(serial_code!=''), CHECK(label!='')); -- This table will be auto-populated as sensors are found on the 1-wire bus.

-- Timestamps are the primary keys for the *_log tables, but each monitoring daemon gets its own table, so timestamps are unique only intra-daemon, not inter-daemon, so collisions won't happen. And even in case they do, centiseconds are finer than the end application's necessary resolution, so insert_record blurs them when necessary to uniquify the keys.
-- Sensor readings are recorded only when they change, to avoid bloating the DB with redundant data. Readings regularly fluctuate, so liveness can reliably be determined by recency of the latest logs. But if they didn't, then a solution would be a per-sensor heartbeat (with period of e.g. 15 seconds), with logging upon a heartbeat, or when readings change, whichever occurs first, so that a gap of more than 15 seconds would mean the system was down, or the sensor was down (or returning implausible readings, filtered out by the daemon).
CREATE TABLE DS18B20_logs(sec INT, cs INT, sensor_ID INT, temp INT, PRIMARY KEY (sec, cs, sensor_ID), FOREIGN KEY (sensor_ID) REFERENCES DS18B20_IDs(sensor_ID)) WITHOUT ROWID; -- sec counts from Unix epoch. cs is approximate centiseconds (exactly timespec.tv_nsec >> 23); encodable in one signed byte. temp is in cC (centi-Celsius), so that each usually takes only 2 bytes, except when negative (since Sqlite doesn't compactly encode negatives).
CREATE TABLE MAX11201B_logs(sec INT, cs INT, flux INT, PRIMARY KEY (sec, cs)) WITHOUT ROWID; -- Flux is in W/m²
CREATE TABLE Furnace_logs(sec INT, cs INT, q1 INT, q2 INT, PRIMARY KEY (sec, cs)) WITHOUT ROWID; -- q1 and q2 are both 1 if furnace has no power. One of q1 or q2 is 0 is furnace has power, but is off. Both are 0 if furnace is on.
CREATE TABLE BME680_logs(sec INT, cs INT, temp INT, pres INT, hum INT, gas INT, PRIMARY KEY (sec, cs)) WITHOUT ROWID; -- temp is in cC, pres in daPa, hum in permil, and gas in kΩ
CREATE TABLE SHT31_logs(sec INT, cs INT, temp INT, hum INT, PRIMARY KEY (sec, cs)) WITHOUT ROWID; -- temp is in cC, hum is in permil
CREATE TABLE TSL2591_logs(sec INT, cs INT, total INT, ired INT, PRIMARY KEY (sec, cs)) WITHOUT ROWID; -- total and ired are each in μW/m², so each takes 1 to 4 bytes
CREATE TABLE VEML6075_logs(sec INT, cs INT, uva INT, uvb INT, PRIMARY KEY (sec, cs)) WITHOUT ROWID; -- uva and uvb are each in mW/m², so each takes 1 to 3 bytes
CREATE TABLE INA260_logs(sec INT, cs INT, Vrms INT, Irms INT, Pmean INT, PRIMARY KEY (sec, cs)) WITHOUT ROWID; -- Vrms is in V, Irms in cA, and Pmean in W.

CREATE VIEW DS18B20_logs_labeled AS SELECT sec, cs, label, temp from DS18B20_logs JOIN DS18B20_IDs ON DS18B20_logs.sensor_ID = DS18B20_IDs.sensor_ID;
CREATE VIEW Furnace_logs_state AS SELECT sec, cs, (q1=0 and q2=0) AS furnace_ctrl, (q1=0 or q2=0) AS furnace_power from Furnace_logs;
CREATE VIEW TSL2591_logs_lux AS SELECT *, (total - ired) AS white, ((total - ired) * 683.0 / 1000000) AS lux from TSL2591_logs;

CREATE VIEW DS18B20_logs_formatted AS SELECT sec, cs, strftime('%Y-%m-%d %H:%M:%S', sec, 'unixepoch', 'localtime') as Timestamp, label, temp/100.0 as Celsius, temp/100.0*9/5+32 AS Fahrenheit from DS18B20_logs_labeled;
CREATE VIEW MAX11201B_logs_formatted AS SELECT sec, cs, strftime('%Y-%m-%d %H:%M:%S', sec, 'unixepoch', 'localtime') as Timestamp, flux AS Flux_W_per_sq_m from MAX11201B_logs;
CREATE VIEW Furnace_logs_state_formatted AS SELECT sec, cs, strftime('%Y-%m-%d %H:%M:%S', sec, 'unixepoch', 'localtime') as Timestamp, CASE furnace_ctrl WHEN 1 THEN "on" WHEN 0 THEN "off" END AS Control_state, CASE furnace_power WHEN 1 THEN "live" WHEN 0 THEN "dead" END AS Power_state from Furnace_logs_state;
CREATE VIEW BME680_logs_formatted AS SELECT sec, cs, strftime('%Y-%m-%d %H:%M:%S', sec, 'unixepoch', 'localtime') as Timestamp, temp/100.0 AS Celsius, temp/100.0*9/5+32 AS Fahrenheit, pres/10.0 as Pressure_mbar, hum/10.0 AS Humidity_percent, gas AS Gas_raw_kohm from BME680_logs;
CREATE VIEW SHT31_logs_formatted AS SELECT sec, cs, strftime('%Y-%m-%d %H:%M:%S', sec, 'unixepoch', 'localtime') as Timestamp, temp/100.0 AS Celsius, temp/100.0*9/5+32 AS Fahrenheit, hum/10.0 as Humidity_percent from SHT31_logs;
CREATE VIEW TSL2591_logs_lux_formatted AS SELECT sec, cs, strftime('%Y-%m-%d %H:%M:%S', sec, 'unixepoch', 'localtime') as Timestamp, total/1000000.0 AS Ptotal_W_per_sq_m, ired/1000000.0 AS Pinfrared_W_per_sq_m, white/1000000.0 AS Pvisible_W_per_sq_m, lux from TSL2591_logs_lux;
CREATE VIEW VEML6075_logs_formatted AS SELECT sec, cs, strftime('%Y-%m-%d %H:%M:%S', sec, 'unixepoch', 'localtime') as Timestamp, uva/1000.0 AS Puva_W_per_sq_m, uvb/1000.0 AS Puvb_W_per_sq_m from VEML6075_logs;
CREATE VIEW INA260_logs_formatted AS SELECT sec, cs, strftime('%Y-%m-%d %H:%M:%S', sec, 'unixepoch', 'localtime') as Timestamp, Vrms AS Vrms_V, Irms/100.0 AS Irms_A, Pmean AS Pmean_W, Vrms*(Irms/100.0) AS Papparent_W, Pmean/(Vrms*(Irms/100.0)) AS Power_factor from INA260_logs;

/* Gets only sec, without matching cs
CREATE VIEW Last_log_sec AS SELECT max(
IFNULL ((SELECT sec from DS18B20_logs ORDER BY sec DESC LIMIT 1), 0),
IFNULL ((SELECT sec from MAX11201B_logs ORDER BY sec DESC LIMIT 1), 0),
IFNULL ((SELECT sec from Furnace_logs ORDER BY sec DESC LIMIT 1), 0),
IFNULL ((SELECT sec from BME680_logs ORDER BY sec DESC LIMIT 1), 0),
IFNULL ((SELECT sec from SHT31_logs ORDER BY sec DESC LIMIT 1), 0),
IFNULL ((SELECT sec from TSL2591_logs ORDER BY sec DESC LIMIT 1), 0),
IFNULL ((SELECT sec from VEML6075_logs ORDER BY sec DESC LIMIT 1), 0),
IFNULL ((SELECT sec from INA260_logs ORDER BY sec DESC LIMIT 1), 0)) AS sec; */

-- What kind of deranged hippies designed this language, anyway??
CREATE VIEW Last_log_times AS
SELECT * FROM
(SELECT sec,cs from DS18B20_logs ORDER BY sec DESC, cs DESC LIMIT 1)
UNION
SELECT * FROM
(SELECT sec,cs from MAX11201B_logs ORDER BY sec DESC, cs DESC LIMIT 1)
UNION
SELECT * FROM
(SELECT sec,cs from Furnace_logs ORDER BY sec DESC, cs DESC LIMIT 1)
UNION
SELECT * FROM
(SELECT sec,cs from BME680_logs ORDER BY sec DESC, cs DESC LIMIT 1)
UNION
SELECT * FROM
(SELECT sec,cs from SHT31_logs ORDER BY sec DESC, cs DESC LIMIT 1)
UNION
SELECT * FROM
(SELECT sec,cs from TSL2591_logs ORDER BY sec DESC, cs DESC LIMIT 1)
UNION
SELECT * FROM
(SELECT sec,cs from VEML6075_logs ORDER BY sec DESC, cs DESC LIMIT 1)
UNION
SELECT * FROM
(SELECT sec,cs from INA260_logs ORDER BY sec DESC, cs DESC LIMIT 1);

CREATE VIEW Last_log_time AS
SELECT IFNULL(sec,0) AS sec, IFNULL(cs,0) AS cs from Last_log_times ORDER BY sec DESC, cs DESC LIMIT 1;

-- This excludes DS18B20_logs, since getting the last temperature for each DS18B20 would have to result in a variable-arity view. Sqlite doesn't support that (or even ordinary PIVOT).
CREATE VIEW Last_logs AS
SELECT * FROM
(SELECT sec
FROM Last_log_time)
JOIN
(SELECT val AS elevation
FROM Config WHERE var='elevation')
JOIN
(SELECT val AS flux_area
FROM Config WHERE var='flux_area')
JOIN
(SELECT val AS exch_flow_rate
FROM Config WHERE var='exch_flow_rate')
JOIN
(SELECT flux AS tflux
FROM MAX11201B_logs ORDER BY sec DESC, cs DESC LIMIT 1)
JOIN
(SELECT furnace_ctrl, furnace_power
FROM Furnace_logs_state ORDER BY sec DESC, cs DESC LIMIT 1)
JOIN
(SELECT temp AS tmp_ctrl_module, pres AS plocal, hum AS hum_in, gas
FROM BME680_logs ORDER BY sec DESC, cs DESC LIMIT 1)
JOIN
(SELECT temp AS tmp_outdoor_module, hum AS hum_out
FROM SHT31_logs ORDER BY sec DESC, cs DESC LIMIT 1)
JOIN
(SELECT total AS vis_ir, ired
FROM TSL2591_logs ORDER BY sec DESC, cs DESC LIMIT 1)
JOIN
(SELECT uva, uvb
FROM VEML6075_logs ORDER BY sec DESC, cs DESC LIMIT 1)
JOIN
(SELECT Vrms, Irms, Pmean
FROM INA260_logs ORDER BY sec DESC, cs DESC LIMIT 1);
