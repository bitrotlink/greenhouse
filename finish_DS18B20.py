#!/usr/bin/python
import os
import glob
import time
from datetime import datetime
import sys
import sqlite3 as sql

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
        print("Usage: finish_DS18B20 archive-database-file")
        sys.exit(1)
    global connArch
    global curArch
    try:
        connArch=sql.connect(sys.argv[1], timeout=5.0)
        curArch=connArch.cursor()
    except sql.Error, e:
        print("Failed to open database file "+sys.argv[1])
        sys.exit(1)

setup()
tim = time.time()
sec = int(tim)
cs = int((tim - sec)*100)
cs += 1 # XXX: preemptively fudge timestamp to uniquify it
if(cs >= 100):
    cs = 0
    sec +=1
curArch.execute("SELECT sensor_ID FROM DS18B20_IDs")
rows=curArch.fetchall()
for row in rows:
    curArch.execute("INSERT INTO DS18B20_logs VALUES(?,?,?,null)", (sec, cs, row[0]))
ensureCommit(connArch)
