#!/usr/bin/python
# -*- coding: utf-8 -*-

#Adapted from https://learn.adafruit.com/adafruits-raspberry-pi-lesson-11-ds18b20-temperature-sensing?view=all

import os
import glob
import time
from datetime import datetime
import sys
import sqlite3 as sql

base_dir = '/sys/bus/w1/devices/'

def ensureCommit(conn):
    commitRetry = 0
    while True:
        try:
            conn.commit()
        except sql.Error, e: #DB locked; retry
            print(e)
            print("Commit failed because database was locked on attempt #%d. Retrying." % commitRetry)
            sys.stdout.flush()
            commitRetry+=1
            continue
        if(commitRetry>0):
            print("Commit finally successful.")
            sys.stdout.flush()
        break

def setup():
    if(len(sys.argv)!=2):
        print("Usage: read_DS18B20 archive-database-file")
        sys.exit(1)
    global connArch
    global curArch
    try:
        connArch=sql.connect(sys.argv[1], timeout=5.0)
        curArch=connArch.cursor()
    except sql.Error, e:
        print("Failed to open database file "+sys.argv[1])
        sys.exit(1)
    print("Starting read_DS18B20.py")
    sys.stdout.flush()

def read_temp_raw(dev_file):
    try:
        f = open(dev_file, 'r')
        lines = f.readlines()
        f.close()
    except:
        return ""
    return lines

def read_temp(dev_folder):
    lines = read_temp_raw(dev_folder+'/w1_slave')
    dev_name = os.path.basename(dev_folder)
    tim = time.time()
    if lines!= "" and lines[0].strip()[-3:] == 'YES':
        equals_pos = lines[1].find('t=')
        if equals_pos != -1:
            temp_string = lines[1][equals_pos+2:]
            temp = int(temp_string)
            if(temp==85000): # The retarded hippies who designed the DS18B20 couldn't be bothered to choose an actual invalid temperature as the chip's power-on reset value, so there's no way to know for sure whether an 85°C reading is actually valid or not.
                print("Read 85°C for %s at time %d; assuming invalid" % (dev_name, tim))
            elif(temp<-55000 or temp>125000):
                print("Read invalid temp %d°C for %s at time %d" % (temp/1000, dev_name, tim))
            else:
                return (int(tim), int((tim - int(tim))*100), dev_name, temp/10)
        else:
            print("Error parsing output for %s at time %d" % (dev_name, tim))
    else:
        print("CRC error for %s at time %d" % (dev_name, tim))
    sys.stdout.flush()
    return ()

prev_readings = {}
live_sensors = {}

setup()
while True:
    dev_folders = glob.glob(base_dir + '28*') #Each time through loop, not just once, since devices can be hot plugged and unplugged, resulting in dirs appearing and disappearing
    results = []
    live_sensors = {}
    for dev_folder in dev_folders: #This takes a while
        res = read_temp(dev_folder)
        if(res!=()):
            # Timestamp duplication prevention no longer necessary, since schema changed to include sensor_ID in the PK for DS18B20_logs
            # if(len(results)>0 and (results[-1][0]==res[0]) and (results[-1][1]==res[1])):
            #     print("Duplicate timestamp!")
            #     sys.stdout.flush()
            #     # Fudge the timestamp to uniquify it
            #     # XXX: This fix fails if >2 consecutive identical timestamps
            #     res[1]+= 1
            #     if(res[1] >= 100):
            #         res[1] = 0
            #         res[0] += 1
            dev_name = res[2]
            temp = res[3]
            live_sensors[dev_name] = True
            if(dev_name not in prev_readings): # Found new device
                prev_readings[dev_name] = None
            if(prev_readings[dev_name]!=temp): # Reading changed
                if((prev_readings[dev_name]==None) and (temp!=None)):
                    print("Sensor %s appeared" % (dev_name,))
                    sys.stdout.flush()
                prev_readings[dev_name]=temp
                results.append(res)
    for res in results: #This is a separate loop to avoid prolonged locking of db
        curArch.execute("INSERT OR IGNORE INTO DS18B20_IDs (serial_code, label) VALUES(?, ?)", (res[2],res[2]))
        curArch.execute("INSERT INTO DS18B20_logs VALUES(?,?,(SELECT sensor_ID from DS18B20_IDs WHERE serial_code=?),?)", (res[0], res[1], res[2], res[3]))
    tim = time.time()
    sec = int(tim)
    cs = int((tim - sec)*100)
    need_flush = False
    for sensor in prev_readings:
        if(prev_readings[sensor]!=None and sensor not in live_sensors):
            prev_readings[sensor] = None
            print("Sensor %s disappeared" % (sensor,))
            need_flush = True
            curArch.execute("INSERT INTO DS18B20_logs VALUES(?,?,(SELECT sensor_ID from DS18B20_IDs WHERE serial_code=?),null)", (sec, cs, sensor))
    ensureCommit(connArch)
    if(need_flush):
        sys.stdout.flush()
