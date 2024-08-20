import socket
import threading
import pyaudio
import queue
import cv2
import numpy as np
import time
import base64

# UDP settings
UDP_IP = "127.0.0.1"
VIDEO_PORT = 9633
AUDIO_PORT = 9999

# Create UDP sockets
video_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
audio_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)


def audio_stream_UDP():
    BUFF_SIZE = 65536
    client_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    client_socket.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, BUFF_SIZE)
    paudio = pyaudio.PyAudio()
    #chunk = 10 * 1024
    chunk_duration = 0.41
    chunk_samples = int(chunk_duration * 1024)
    stream = paudio.open(format=paudio.get_format_from_width(2),
                         channels=2,
                         rate=44100,
                         output=True,
                         frames_per_buffer=chunk_samples)

    # create socket
    host_name = socket.gethostname()
    host_ip = socket.gethostbyname(host_name)
    print(host_ip)
    port_audio = 9633
    message = b'Hello'
    client_socket.sendto(message, (host_ip, port_audio))
    DATA_SIZE, _ = client_socket.recvfrom(BUFF_SIZE)
    DATA_SIZE = int(DATA_SIZE.decode())
    q = queue.Queue(maxsize=DATA_SIZE)
    cnt = 0

    def getAudioData():
        while True:
            frame, _ = client_socket.recvfrom(BUFF_SIZE)
            q.put(frame)
            print('[Queue size while loading]...', q.qsize())

    t1 = threading.Thread(target=getAudioData, args=())
    t1.start()
    time.sleep(5)
    frame_no = 1
    DURATION = DATA_SIZE * chunk_samples / 44100
    print('[Now Playing]... Data', DATA_SIZE, '[Audio Time]:', DURATION, 'seconds')
    while True:
        frame = q.get()
        if frame:
            with open("audiotest.txt", "a") as myfile:
                print("for frame : " + str(frame_no) + "   timestamp is: ", str(time.time()), file=myfile)
                myfile.close()
            frame_no = frame_no + 1
        stream.write(frame)
        print('[Queue size while playing]...', q.qsize(), '[Time remaining...]', round(DURATION), 'seconds')
        DURATION -= chunk_samples / 44100
    client_socket.close()
    print('Audio closed')
    os._exit(1)

def video_stream_UDP():
    BUFF_SIZE = 65536
    client_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    client_socket.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, BUFF_SIZE)
    video_host_name = socket.gethostname()
    video_host_ip = socket.gethostbyname(video_host_name)
    print(video_host_ip)
    port_video = 9999
    message = b'Hello'

    client_socket.sendto(message, (video_host_ip, port_video))
    fps, st, frames_to_count, cnt = (0, 0, 20, 0)
    frame_no = 1
    while True:
        packet, _ = client_socket.recvfrom(BUFF_SIZE)
        packet_data = base64.b64decode(packet, ' /')
        np_data = np.fromstring(packet_data, dtype=np.uint8)
        frame = cv2.imdecode(np_data, 1)
        if frame.any():
            with open("videotest.txt", "a") as myfile:
                print("for frame : " + str(frame_no) + "   timestamp is: ", str(time.time()), file=myfile)
                myfile.close()
            frame_no = frame_no + 1
        frame = cv2.putText(frame, 'FPS: ' + str(fps), (10, 40), cv2.FONT_HERSHEY_SIMPLEX, 0.7, (0, 0, 255), 2)
        cv2.imshow("RECEIVING VIDEO", frame)
        waitkey = cv2.waitKey(50) & 0xFF
        if waitkey == ord('e'):
            client_socket.close()
            break
        if cnt == frames_to_count:
            try:
                fps = round(frames_to_count / (time.time() - st))
                st = time.time()
                cnt = 0
            except:
                pass
        cnt += 1


cv2.destroyAllWindows()
def sync_streams():
    while True:
        # Example marker for synchronization
        marker = b'SYNC_MARKER'
        video_socket.sendto(marker, (UDP_IP, VIDEO_PORT))
        audio_socket.sendto(marker, (UDP_IP, AUDIO_PORT))
        time.sleep(1)  # Adjust timing as necessary


video_thread = threading.Thread(target=video_stream_UDP)
audio_thread = threading.Thread(target=audio_stream_UDP)
sync_thread = threading.Thread(target=sync_streams)

audio_thread.start()
video_thread.start()
sync_thread.start()

video_thread.join()
audio_thread.join()
sync_thread.join()

cv2.destroyAllWindows()