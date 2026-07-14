import os
import re
import socket
import subprocess
import tempfile
import time


HOST = "127.0.0.1"
PORT = 45683


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


def connect_client():
    sock = socket.create_connection((HOST, PORT), timeout=2)
    time.sleep(0.1)
    read_some(sock)
    return sock


def start_server(state_file):
    return subprocess.Popen(
        ["./multi_server", str(PORT), state_file],
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

            send_line(owner, "/create notice")
            send_line(member, "/join notice")
            read_some(owner)
            read_some(member)

            send_line(member, "hello")
            owner_text = read_some(owner)
            assert re.search(r"\[notice\] member: hello \{\d{2}:\d{2}\}", owner_text)
            assert "{#1}" not in owner_text

            send_line(member, "/profile backend")
            send_line(member, "/status away")
            read_some(member)
            send_line(owner, "/profileof member")
            profile_text = read_some(owner)
            assert "profile=backend" in profile_text
            assert "status=away" in profile_text

            send_line(member, "/status_public off")
            read_some(member)
            send_line(owner, "/profileof member")
            assert "profile=backend status=hidden" in read_some(owner)

            send_line(member, "/profile_public off")
            read_some(member)
            send_line(owner, "/profileof member")
            assert "profile=private status=hidden" in read_some(owner)

            send_line(member, "/leave")
            assert "joined lobby" in read_some(member)
            read_some(owner)  # drain leave notification before checking /who output
            send_line(owner, "/who")
            assert "member" not in read_some(owner)

            send_line(guest, "/join notice")
            read_some(guest)
            send_line(owner, "/kick guest")
            kicked_owner_text = read_some(owner)
            kicked_guest_text = read_some(guest)
            assert "guest 강퇴 처리" in kicked_owner_text
            assert "강퇴되어 lobby로 이동했습니다" in kicked_guest_text
            send_line(owner, "/who")
            assert "guest" not in read_some(owner)
        finally:
            owner.close()
            member.close()
            guest.close()
    finally:
        stop_server(server)
