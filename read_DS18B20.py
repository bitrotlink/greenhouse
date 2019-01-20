#!/usr/bin/python

#Adapted from https://learn.adafruit.com/adafruits-raspberry-pi-lesson-11-ds18b20-temperature-sensing?view=all

import os
import glob
import time
from datetime import datetime
import sys
import sqlite3 as sql

#os.system('modprobe w1-gpio')
#os.system('modprobe w1-therm')

base_dir = '/sys/bus/w1/devices/'

def ensureCommit(conn):
    commitRetry = 0
    while True:
        try:
            conn.commit()
        except sql.Error, e: #DB locked; retry
            commitRetry+=1
            print(e)
            print("Commit failed because database is locked! Retry #%d..." % commitRetry)
            continue
        if(commitRetry>0):
            print("Commit finally successful.")
        break

def setup():
    if(len(sys.argv)!=3):
        print("Usage: read_DS18B20 archive-database-file volatile-database-file")
        print("Example: read_DS18B20 arch.db volatile.db")
        sys.exit(1)
    global connArch
    global curArch
    global connVolatile
    global curVolatile
    try:
        connArch=sql.connect(sys.argv[1], timeout=5.0)
        curArch=connArch.cursor()
    except sql.Error, e:
        print("Failed to open database file "+sys.argv[1])
        sys.exit(1)
    try:
        connVolativec=sql.connect(sys.argv[2], timeout=5.0)
        curVolatile=connVolativec.cursor()
    except sql.Error, e:
        print("Failed to open database file "+sys.argv[2])
        sys.exit(1)
    try:
        curArch.executescript("""
        CREATE TABLE IF NOT EXISTS Sensors(Sensor_ID INTEGER PRIMARY KEY, Sensor_Global_Id TEXT, Label TEXT);
        CREATE TABLE IF NOT EXISTS Sensor_logs(Event_ID INTEGER PRIMARY KEY, Sensor_ID INT, Val REAL, Timestamp INT);
        """)
    except sql.Error, e:
        print("Failed to initialize database.")
        print(e)
        sys.exit(1)
    try:
        curVolatile.executescript("""
        CREATE TABLE IF NOT EXISTS Sensors(Sensor_ID INTEGER PRIMARY KEY, Sensor_Global_Id TEXT, Label TEXT);
        CREATE TABLE IF NOT EXISTS Sensor_logs(Event_ID INTEGER PRIMARY KEY, Sensor_ID INT, Val REAL, Timestamp INT);
""")
    except sql.Error, e:
        print("Failed to initialize database.")
        print(e)
        sys.exit(1)

def read_temp_raw(device_file):
    try:
        f = open(device_file, 'r')
        lines = f.readlines()
        f.close()
    except:
        return ""
    return lines

def read_temp(device_folder):
    lines = read_temp_raw(device_folder+'/w1_slave')
    device_name = os.path.basename(device_folder)
    if lines!= "" and lines[0].strip()[-3:] == 'YES':
        equals_pos = lines[1].find('t=')
        if equals_pos != -1:
            temp_string = lines[1][equals_pos+2:]
            temp_c = float(temp_string) / 1000.0
            temp_f = temp_c * 9.0 / 5.0 + 32.0
            return (device_name, temp_c, int(time.time()))
    return ()

setup()
while True:
    device_folders = glob.glob(base_dir + '28*') #Each time through loop, not just once, since devices can be hot plugged and unplugged, resulting in dirs appearing and disappearing
    results = []
    for device_folder in device_folders: #This takes a while
        result = read_temp(device_folder)
        if(result!=()):
            results.append(result)
    for result in results: #This is separate loop to avoid prolonged locking of db
        curArch.execute("SELECT Sensor_Id from Sensors where Sensor_Global_ID=?", (result[0],))
        rows=curArch.fetchall()
        if ((len(rows)==0)): #Device not already listed, so add it here
            curArch.execute("INSERT INTO Sensors (Sensor_Global_Id, Label) VALUES(?, ?)", (result[0],""))
        curArch.execute("INSERT INTO Sensor_logs(Sensor_ID, Val, Timestamp) VALUES((SELECT Sensor_ID from Sensors WHERE Sensor_Global_Id=?), ?, ?)", (result[0], result[1], result[2]))
    ensureCommit(connArch)
#    time.sleep(1)
