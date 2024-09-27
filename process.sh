#!/bin/bash

python3 mulserver.py&

sleep 30s

cat demo.txt | cut -d" " -f 4- | sed 's/[b]//g' | sed -e "s/'//g" > Packetinfo.txt

awk  '{ print $0, $13 = $10 - $7 }' Packetinfo.txt  > prefinal.txt

echo "ConnectionName   PktNumber   OutgoingTime     ReceivingTime   difference " > Final.csv

echo " " >> Final.csv

awk  '{ print $1"   "$2"    "$7"     "$10"   "$13 }' prefinal.txt >> Final.csv

sudo kill -9 $(ps -e |grep python)

clear

exit 0
