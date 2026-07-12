import socket
import subprocess
import time


HOST = "127.0.0.1"
PORT = 45678


def read_some(sock):
    sock.settimeout(2)
    chunks = []
    start = time.time()
    while time.time() - start < 0.3:
        try:
            chunks.append(sock.recv(4096).decode("utf-8"))
        except socket.timeout:
            break
    return "".join(chunks)


def send_line(sock, line):
    sock.sendall((line + "\n").encode("utf-8"))
    time.sleep(0.2)


server = subprocess.Popen(["./multi_chat_server", str(PORT)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
try:
    time.sleep(0.3)
    a = socket.create_connection((HOST, PORT), timeout=2)
    b = socket.create_connection((HOST, PORT), timeout=2)
    try:
        read_some(a)
        read_some(b)

        send_line(a, "/nick alice")
        send_line(b, "/nick bob")
        send_line(a, "/profile backend")
        assert "프로필이 설정되었습니다" in read_some(a)

        send_line(a, "/create study")
        assert "study" in read_some(a)

        send_line(b, "/join study")
        assert "study" in read_some(b)

        send_line(a, "hello")
        a_text = read_some(a)
        b_text = read_some(b)
        assert "[study] alice: hello" in a_text
        assert "[study] alice: hello" in b_text

        send_line(b, "/who")
        who_text = read_some(b)
        assert "alice" in who_text
        assert "bob" in who_text
    finally:
        a.close()
        b.close()
finally:
    server.terminate()
    try:
        server.wait(timeout=2)
    except subprocess.TimeoutExpired:
        server.kill()
