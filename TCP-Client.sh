#!/bin/bash

sudo kill -9 $(ps -e |grep python| awk '{print $1}')

python3 client.py&

exit 0

