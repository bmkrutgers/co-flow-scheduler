# Server code to stream both audio and video
import math
import socket
import threading
import time
import wave

import base64
import cv2
import imutils

port_audio = 9633
port_video = 9999
BUFF_SIZE = 65536


def split_audio(file_path, chunk_size):
    audio_chunks = []
    with wave.open(file_path, 'rb') as wf:
        frame_rate = wf.getframerate()
        n_channels = wf.getnchannels()
        n_frames = wf.getnframes()
        print('total frames in audio is ', n_frames)
        data_size = n_frames / chunk_size

        for i in range(0, n_frames, chunk_size):
            wf.setpos(i)
            frames = wf.readframes(min(chunk_size, n_frames - i))
            audio_chunks.append(frames)

    return audio_chunks, frame_rate, n_channels, data_size


def audio_stream_UDP():
    host_name = socket.gethostname()
    host_ip = socket.gethostbyname(host_name)  # '127.0.0.1'
    print(host_ip)
    server_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    server_socket.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, BUFF_SIZE)
    audio_socket_address = (host_ip, port_audio)
    print('Audio stream server Listening at:', audio_socket_address)
    server_socket.bind((host_ip, port_audio))

    individual_chunk_size = 1764

    audio_chunks, audio_frame_rate, audio_n_channels, sample_data_size = split_audio("audio.wav", individual_chunk_size)

    while True:
        msg, client_addr = server_socket.recvfrom(BUFF_SIZE)
        print('Connected To ----> ', client_addr, msg)
        sample_data_size = math.ceil(sample_data_size)
        sample_data_size = str(sample_data_size).encode()
        print('[Sending data size]...', sample_data_size)
        server_socket.sendto(sample_data_size, client_addr)
        for chunk in audio_chunks:
            server_socket.sendto(chunk, client_addr)
            time.sleep(0.001)





    print('SENT...')

def video_stream_UDP():
    server_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    server_socket.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, BUFF_SIZE)
    video_host_name = socket.gethostname()
    video_host_ip = socket.gethostbyname(video_host_name)  # ip name
    print(video_host_ip)
    port_video = 9999
    socket_address = (video_host_ip, port_video)
    server_socket.bind(socket_address)
    print('video stream server Listening at:', socket_address)

    video_sample = cv2.VideoCapture('noaudio.mp4')  # replace 'rocket.mp4' with 0 for webcam
    print("total number of video frames are :", video_sample.get(cv2.CAP_PROP_FRAME_COUNT))
    frps = video_sample.get(cv2.CAP_PROP_FPS)
    print("video frames per second:", frps)
    total_no_frames = video_sample.get(cv2.CAP_PROP_FRAME_COUNT)
    duration_in_seconds = total_no_frames // frps

    print("Video Duration In Seconds:", duration_in_seconds, "s")

    fps, st, frames_to_count, frame_count = (0, 0, 20, 0)

    while True:
        msg, video_client_addr = server_socket.recvfrom(BUFF_SIZE)
        print('Connected to ---> ', video_client_addr)
        frame_width = 400
        frame_no = 0
        while (video_sample.isOpened()):
            _, frame = video_sample.read()
            if frame.any():
                print("for frame : " + str(frame_no) + "   timestamp is: ", str(video_sample.get(cv2.CAP_PROP_POS_MSEC)))
            else:
                break
            frame_no += 1
            frame = imutils.resize(frame, width=frame_width)
            encoded, video_buffer = cv2.imencode('.jpg', frame, [cv2.IMWRITE_JPEG_QUALITY, 80])
            video_message = base64.b64encode(video_buffer)
            server_socket.sendto(video_message, video_client_addr)
            frame = cv2.putText(frame,'FPS: '+str(fps),(10,40),cv2.FONT_HERSHEY_SIMPLEX,0.7,(0,0,255),2)
            cv2.imshow('TVideo Transmission',frame)
            key = cv2.waitKey(50) & 0xFF
            if key == ord('e'):
                server_socket.close()
                break
            if frame_count == frames_to_count:
                try:
                    fps = round(frames_to_count / (time.time() - st))
                    st = time.time()
                    frame_count = 0
                except:
                    pass
            frame_count += 1


audio_thread = threading.Thread(target=audio_stream_UDP, args=())
video_thread = threading.Thread(target=video_stream_UDP, args=())

audio_thread.start()
time.sleep(0.5)
video_thread.start()

audio_thread.join()
video_thread.join()


exit()
