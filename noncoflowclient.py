#!/usr/bin/python
# -*- coding: utf-8 -*-
import socket
import random
import time
from scapy.all import *

host = '127.0.0.1'
port = 12347

SOURCE_PORT_Three = 46732

print ('Client3 running')

ClientSocket_Three = socket.socket(family=socket.AF_INET,
                                   type=socket.SOCK_DGRAM)
ClientSocket_Three.bind(('0.0.0.0', SOURCE_PORT_Three))

serverAddressPort = ('127.0.0.1', 12347)

# Input = input('Your choice: ')
# Input = int(Input)

Input = 60
count = 0
packetthree = 0

while count < Input:

    # element = input('Your choice: ')
    # element = int(element)

    element = 3
    print ('input value is', element)
    if element == 3:
        time_nanosec = time.time_ns()
        data = 'Connection_Thr ' + str(packetthree + 1) \
            + ' Packet with Outgoing Time ' + str(time_nanosec)
        packet = data.encode()
        ClientSocket_Three.sendto(packet, serverAddressPort)
        time.sleep(0.05)
        count += 1
        packetthree += 1
    else:

        print ('invalid choice try again')
        count += 1
ClientSocket_Three.close()

# exit()
