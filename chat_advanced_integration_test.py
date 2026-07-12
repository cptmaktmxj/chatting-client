import os
import socket
import subprocess
import tempfile
import time


HOST = "127.0.0.1"
PORT = 45679


def read_some(sock, seconds=0.25):
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


def connect_client():
    sock = socket.create_connection((HOST, PORT), timeout=2)
    time.sleep(0.1)
    read_some(sock)
    return sock


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


with tempfile.TemporaryDirectory() as tempdir:
    state_file = os.path.join(tempdir, "chat_state.db")
    server = start_server(state_file)
    try:
        time.sleep(0.3)
        owner = connect_client()
        member = connect_client()
        guest = connect_client()
        try:
            send_line(owner, "/nick owner")
            send_line(member, "/nick member")
            send_line(guest, "/nick guest")
            read_some(owner)
            read_some(member)
            read_some(guest)

            send_line(owner, "/create_hidden secret")
            send_line(member, "/join secret")
            read_some(owner)
            read_some(member)
            send_line(owner, "hidden hello")
            assert "[secret] anonymous: hidden hello" in read_some(member)

            send_line(owner, "/openlink counsel")
            assert "counsel" in read_some(owner)
            send_line(member, "/enterlink counsel")
            assert "counsel_member" in read_some(member)
            send_line(member, "private question")
            owner_text = read_some(owner)
            guest_text = read_some(guest)
            assert "private question" in owner_text
            assert "private question" not in guest_text

            send_line(owner, "/create_announce notice")
            send_line(member, "/join notice")
            read_some(owner)
            read_some(member)
            send_line(member, "not allowed")
            assert "말할 권한이 없습니다" in read_some(member)
            send_line(owner, "official notice")
            assert "official notice" in read_some(member)

            send_line(owner, "/create_approval gate")
            send_line(member, "/join gate")
            read_some(owner)
            read_some(member)
            send_line(member, "blocked")
            assert "말할 권한이 없습니다" in read_some(member)
            send_line(owner, "/permit member")
            assert "member" in read_some(owner)
            send_line(member, "allowed")
            assert "allowed" in read_some(owner)

            send_line(owner, "/delegate member")
            assert "member" in read_some(owner)
            send_line(member, "/kick guest")
            assert "guest" in read_some(member)

            send_line(owner, "/create_secure vault swordfish")
            assert "vault" in read_some(owner)
            send_line(guest, "/join_secure vault wrong")
            assert "비밀번호가 틀렸습니다" in read_some(guest)
            send_line(guest, "/join_secure vault swordfish")
            assert "vault" in read_some(guest)
        finally:
            owner.close()
            member.close()
            guest.close()
    finally:
        stop_server(server)

    server = start_server(state_file)
    try:
        time.sleep(0.3)
        newcomer = connect_client()
        try:
            send_line(newcomer, "/rooms")
            rooms = read_some(newcomer)
            assert "secret" in rooms
            assert "notice" in rooms
            assert "gate" in rooms
            assert "vault" in rooms
            assert "counsel" in rooms
        finally:
            newcomer.close()
    finally:
        stop_server(server)
