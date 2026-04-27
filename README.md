# TSDB — Time-Series Database Engine

Advanced Database Management — CS 4th Semester, Project 02  
Group 25

## Members
- BSCS24063 : Mubeen Butt
- BSCS24075 : Muhammad Bin Asghar Ghouri
- BSCS24045 : Israr Hussain

## Build
make

## Run
./tsdb --data ./data --port 5555

## Connect
nc localhost 5555

## Commands
PUT metric_name timestamp value
GET metric_name from_ts to_ts
STATS metric_name
QUIT