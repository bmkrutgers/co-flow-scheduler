import socket
import random
host = '127.0.0.1'
portone = 12345
porttwo = 12346
portthree = 12347

ClientSocket_One = socket.socket()
ClientSocket_Two = socket.socket()
ClientSocket_Three = socket.socket()

print('Waiting for connection')
try:
    ClientSocket_One.connect((host, portone))
except socket.error as e:
    print(str(e))
Response_One = ClientSocket_One.recv(2048)
try:
    ClientSocket_Two.connect((host, porttwo))
except socket.error as e:
    print(str(e))
Response_Two = ClientSocket_Two.recv(2048)
try:
    ClientSocket_Three.connect((host, portthree))
except socket.error as e:
    print(str(e))
Response_Three = ClientSocket_Three.recv(2048)

Input = input('Your choice: ')

while True:
    Input = int(Input)
    #chars = list(Input)
    for num in range(Input):
        element = random.randint(1, 4)
        print(element)
        if element == 1:
            ClientSocket_One.send(str.encode("Connection_One"))
            Response_One = ClientSocket_One.recv(2048)
            print(Response_One.decode('utf-8'))

        elif element == 2:
            ClientSocket_Two.send(str.encode("Connection_Two"))
            Response_Two = ClientSocket_Two.recv(2048)
            print(Response_Two.decode('utf-8'))

        elif element == 3:
            ClientSocket_Three.send(str.encode("Connection_Three"))
            Response_Three = ClientSocket_Three.recv(2048)
            print(Response_Three.decode('utf-8'))

        elif element == 4:
            print("exiting the application")
            ClientSocket_One.close()
            ClientSocket_Two.close()
            ClientSocket_Three.close()
            exit()

        else:
            print("invalid choice try again")
