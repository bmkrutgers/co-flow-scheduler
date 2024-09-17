import threading
import time
import socket
import subprocess as sp
import sys
import os

timeout = 5   # [seconds]

timeout_start = time.time()

if os.path.exists("demo.txt"):
  os.remove("demo.txt")


class udpreceive:
     def __init__(self,port,ip):
          self.port = port
          self.ip = ip
          self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
          self.sock.bind((self.ip, self.port))
          self.sock.listen()

     def startserver(self):
          count = 0
          conn, addr = self.sock.accept()
          while True:
          	data = conn.recv(1024).decode("ascii")
          	#print(data)
          	if data:
          		time_nanosec = time.time_ns()
          		sourceFile = open('demo.txt', 'a')
          		print(f'port {self.port} receives {data} Incoming Time  '+ str(time_nanosec) + ' Time Difference', file = sourceFile)
          		sourceFile.close()          
               #if self.port == 12347:
                #   time_nanosec = time.time_ns()
                 #  sourceFile = open('demo.txt', 'a')
                  # print(f'port {self.port} receives {data} Incoming Time  '+ str(time_nanosec) + ' Time Difference', file = sourceFile)
                   #sourceFile.close()
               
s1 = udpreceive(12345, "127.0.0.1")
s2 = udpreceive(12346, "127.0.0.1")
s3 = udpreceive(12347, "127.0.0.1")
threads = [threading.Thread(target=s1.startserver), threading.Thread(target=s2.startserver), threading.Thread(target=s3.startserver)]

for th in threads:
     th.start()
     print(f'threads {th} started')
     th.join(0.1)
     
exit()
