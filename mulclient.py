#!/usr/bin/python
# -*- coding: utf-8 -*-
import socket
import random
import time
#host = '192.168.122.76'
host = '127.0.0.1'
port_one = 12345
port_two = 12346

SOURCE_PORT_ONE = 46730
SOURCE_PORT_TWO = 46731
SOURCE_PORT_THREE = 46732

ClientSocket_One = socket.socket(family=socket.AF_INET,
                                 type=socket.SOCK_DGRAM)
ClientSocket_One.bind(('0.0.0.0', SOURCE_PORT_ONE))
ClientSocket_Two = socket.socket(family=socket.AF_INET,
                                 type=socket.SOCK_DGRAM)
ClientSocket_Two.bind(('0.0.0.0', SOURCE_PORT_TWO))
ClientSocket_Three = socket.socket(family=socket.AF_INET,
                                   type=socket.SOCK_DGRAM)
ClientSocket_Three.bind(('0.0.0.0', SOURCE_PORT_THREE))

serverAddressPort_ONE = ('127.0.0.1', 12345)
serverAddressPort_TWO = ('127.0.0.1', 12346)
serverAddressPort_THREE = ('127.0.0.1', 12347)

# Input = input('Your choice: ')
# Input = int(Input)

Input = 10
count = 0
packetone = 0
packettwo = 0
packetthree = 0

while count < Input:
    #element = input('Your choice: ')
    #element = int(element)
    element = random.randint(1, 3)

    #print ('input value is', element)
    if element == 1:
        time_nanosec = time.time_ns()
        print ('A sent its packet')
        data = 'Connection_A sent its ' + str(packetone + 1) \
            + ' Packet with Outgoing Time ' + str(time_nanosec)
        packet = data.encode()
        ClientSocket_One.sendto(packet, serverAddressPort_ONE)
        count += 1
        packetone += 1
        time.sleep(0.05)
    elif element == 2:

        time_nanosec = time.time_ns()
        print ('B sent its packet')
        data = 'Connection_B sent its ' + str(packettwo + 1) \
            + ' Packet with Outgoing Time ' + str(time_nanosec)
        packet = data.encode()
        ClientSocket_Two.sendto(packet, serverAddressPort_TWO)
        count += 1
        packettwo += 1
        time.sleep(0.05)
    elif element == 3:

        time_nanosec = time.time_ns()
        print ('C sent its packet')
        data = 'Connection_C sent its ' + str(packetthree + 1) \
            + ' Packet with Outgoing Time ' + str(time_nanosec)
        packet = data.encode()
        ClientSocket_Three.sendto(packet, serverAddressPort_THREE)
        count += 1
        packetthree += 1
        time.sleep(0.05)
    elif element == 4:

        print ("exiting the application")
        ClientSocket_One.close()
        ClientSocket_Two.close()
        ClientSocket_Three.close()
        exit()
    else:

        print ("invalid choice try again")
        count += 1
ClientSocket_One.close()
ClientSocket_Two.close()
ClientSocket_Three.close()

exit()

