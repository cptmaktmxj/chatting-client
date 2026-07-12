import os
import socket
import subprocess
import tempfile
import time


HOST = "127.0.0.1"
PORT = 45680


def read_some(sock, seconds=0.3):
    sock.settimeout(0.15)
    chunks = []
    end = time.time() + seconds
    while time.time() < end:
        try:
            data = sock.recv(4096)
            if not data:
                break
            chunks.append(data.decode("utf-8", errors="replace"))
        except socket.timeout:
            pass
    return "".join(chunks)


def send_line(sock, line):
    sock.sendall((line + "\n").encode("utf-8"))
    time.sleep(0.2)


def start_server(state_file):
    return subprocess.Popen(
        ["./multi_chat_server_v2", str(PORT), state_file],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )


def stop_server(proc):
    proc.terminate()
    try:
        proc.wait(timeout=2)
    except subprocess.TimeoutExpired:
        proc.kill()


def connect_client():
    sock = socket.create_connection((HOST, PORT), timeout=2)
    time.sleep(0.1)
    read_some(sock)
    return sock


with tempfile.TemporaryDirectory() as tempdir:
    state_file = os.path.join(tempdir, "chat_state.db")
    server = start_server(state_file)
    try:
        time.sleep(0.3)
        alice = connect_client()
        bob = connect_client()
        try:
            send_line(alice, "/nick alice")
            send_line(bob, "/nick bob")
            read_some(alice)
            read_some(bob)

            send_line(alice, "/template add hi 안녕하세요!")
            assert "template hi saved" in read_some(alice)
            send_line(alice, "/template add ok 네, 알겠습니다.")
            assert "template ok saved" in read_some(alice)
            send_line(alice, "/template suggest h")
            assert "hi = 안녕하세요!" in read_some(alice)
            send_line(alice, "/template list")
            listed = read_some(alice)
            assert "hi = 안녕하세요!" in listed
            assert "ok = 네, 알겠습니다." in listed

            send_line(alice, "/template use hi")
            bob_text = read_some(bob)
            assert "[lobby] alice: 안녕하세요! {#1}" in bob_text

            send_line(bob, "/react 1 👍")
            reaction_text = read_some(alice)
            assert "[reaction] #1 👍 by bob" in reaction_text
            send_line(alice, "/reactions 1")
            assert "bob=👍" in read_some(alice)

            send_line(alice, "/template del ok")
            assert "template ok deleted" in read_some(alice)
        finally:
            alice.close()
            bob.close()
    finally:
        stop_server(server)

    server = start_server(state_file)
    try:
        time.sleep(0.3)
        alice = connect_client()
        try:
            send_line(alice, "/nick alice")
            read_some(alice)
            send_line(alice, "/template list")
            persisted = read_some(alice)
            assert "hi = 안녕하세요!" in persisted
            assert "ok = 네, 알겠습니다." not in persisted
        finally:
            alice.close()
    finally:
        stop_server(server)
