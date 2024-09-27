import socket
import random
import time
host = '127.0.0.1'
port_one = 12345
port_two = 12346



SOURCE_PORT_ONE = 46730
SOURCE_PORT_TWO = 46731


ClientSocket_One = socket.socket(family=socket.AF_INET, type=socket.SOCK_DGRAM)
ClientSocket_One.bind(('0.0.0.0', SOURCE_PORT_ONE))
ClientSocket_Two = socket.socket(family=socket.AF_INET, type=socket.SOCK_DGRAM)
ClientSocket_Two.bind(('0.0.0.0', SOURCE_PORT_TWO))


serverAddressPort_ONE   = ("127.0.0.1", 12345)
serverAddressPort_TWO   = ("127.0.0.1", 12346)
#Input = input('Your choice: ')
#Input = int(Input)
Input = 50
count = 0
packetone = 0
packettwo = 0

while count < Input:
    #element = input('Your choice: ')
    #element = int(element)
    element = random.randint(1, 2)
    print("input value is" , element)
    if element == 1:
        time_nanosec = time.time_ns()
        data = "Connection_One " + str(packetone + 1) + " Packet with Outgoing Time " + str(time_nanosec)
        packet = data.encode()
        ClientSocket_One.sendto(packet,serverAddressPort_ONE)
            #Response_One = ClientSocket_One.recv(2048)
            #print(Response_One.decode('utf-8'))
        count += 1
        packetone += 1
        time.sleep(0.05)

    elif element == 2:
        time_nanosec = time.time_ns()
        data = "Connection_Two " + str(packettwo + 1) + " Packet with Outgoing Time " + str(time_nanosec)
        packet = data.encode()
        ClientSocket_Two.sendto(packet,serverAddressPort_TWO)
            #Response_Two = ClientSocket_Two.recv(2048)
            #print(Response_Two.decode('utf-8'))
        count += 1
        packettwo += 1
        time.sleep(0.05)

    elif element == 4:
            print("exiting the application")
            ClientSocket_One.close()
            ClientSocket_Two.close()
            exit()


    else:
            print("invalid choice try again")
            count += 1
ClientSocket_One.close()
ClientSocket_Two.close()
#exit()

