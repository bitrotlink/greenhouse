#!/bin/bash
echo -e ".headers on\n.mode csv\n.once last_log_data.csv\nselect * from Last_log_data;" | sqlite3 ghpi-arch.db
