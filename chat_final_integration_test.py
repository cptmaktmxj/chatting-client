import os
import re
import socket
import subprocess
import tempfile
import time


HOST = "127.0.0.1"
PORT = 45683


# 소켓에서 짧은 시간 동안 수신 가능한 데이터를 모아 문자열로 반환합니다.
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


# 테스트 클라이언트 소켓으로 한 줄 명령 또는 메시지를 전송합니다.
def send_line(sock, line):
    sock.sendall((line + "\n").encode("utf-8"))
    time.sleep(0.2)


# 테스트용 클라이언트 소켓을 만들고 초기 서버 알림을 비웁니다.
def connect_client():
    sock = socket.create_connection((HOST, PORT), timeout=2)
    time.sleep(0.1)
    read_some(sock)
    return sock


# 임시 상태 파일을 사용하는 테스트 서버 프로세스를 시작합니다.
def start_server(state_file):
    return subprocess.Popen(
        ["./multi_server", str(PORT), state_file],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )


# 테스트 서버 프로세스를 정상 종료하고 필요하면 강제 종료합니다.
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

            send_line(owner, "room owner message")
            read_some(member)
            send_line(owner, "/history")
            history_text = read_some(owner)
            assert "[history notice]" in history_text
            assert "#2" in history_text
            assert "room owner message" in history_text
            send_line(owner, "/search owner")
            assert "room owner message" in read_some(owner)
            send_line(owner, "/since 1")
            assert "#2" in read_some(owner)

            send_line(member, "/join notice")
            read_some(member)
            send_line(member, "/reply 2 확인했습니다")
            reply_text = read_some(owner)
            assert "[reply #2]" in reply_text
            assert "확인했습니다" in reply_text

            send_line(owner, "/pin 2")
            assert "pinned #2" in read_some(owner)
            send_line(member, "/pinned")
            assert "#2" in read_some(member)
            send_line(member, "/unpin 2")
            assert "pin한 사람 또는 방장만" in read_some(member)
            send_line(owner, "/unpin 2")
            assert "unpinned #2" in read_some(owner)

            send_line(owner, "/edit 2 edited owner message")
            assert "edited #2" in read_some(owner)
            send_line(owner, "/delete 2")
            assert "deleted #2" in read_some(owner)

            send_line(owner, "/create_approval gate")
            send_line(member, "/join gate")
            read_some(owner)
            read_some(member)
            send_line(member, "blocked")
            assert "말할 권한이 없습니다" in read_some(member)
            send_line(member, "/request_speak")
            assert "발언 권한을 요청했습니다" in read_some(member)
            assert "speak request" in read_some(owner)
            send_line(owner, "/requests")
            assert "member" in read_some(owner)
            send_line(owner, "/approve member")
            assert "발언 권한 승인" in read_some(owner)
            send_line(member, "allowed")
            assert "allowed" in read_some(owner)

            send_line(owner, "/ban member")
            assert "ban 처리" in read_some(owner)
            assert "차단되어 lobby로 이동했습니다" in read_some(member)
            send_line(member, "/join gate")
            assert "차단된 방입니다" in read_some(member)
            send_line(owner, "/unban member")
            assert "unban 처리" in read_some(owner)
            send_line(member, "/join gate")
            assert "joined gate" in read_some(member)
            send_line(owner, "/transfer_owner member")
            assert "owner transferred to member" in read_some(owner)
            send_line(member, "/transfer_owner owner")
            assert "owner transferred to owner" in read_some(member)

            send_line(owner, "/rename_room gate2")
            assert "renamed gate -> gate2" in read_some(owner)
            send_line(owner, "/set_room_type normal")
            assert "room type set to normal" in read_some(owner)
            send_line(owner, "/delete_room")
            assert "deleted room gate2" in read_some(owner)
            assert "deleted room gate2" in read_some(member)
            send_line(owner, "/rooms")
            assert "gate2" not in read_some(owner)
            send_line(owner, "/who")
            lobby_users = read_some(owner)
            assert "owner" in lobby_users
            assert "member" in lobby_users
            send_line(owner, "/history")
            assert "allowed" not in read_some(owner)

            send_line(owner, "/create_secret vault pw")
            assert "joined vault" in read_some(owner)
            send_line(member, "/join_secret vault wrong")
            assert "비밀번호가 틀렸습니다" in read_some(member)
            send_line(member, "/join_secret vault pw")
            assert "joined vault" in read_some(member)

            send_line(owner, "/openlink ask")
            assert "openlink ask created" in read_some(owner)
            send_line(owner, "/set_link_password ask linkpw")
            assert "openlink password set ask" in read_some(owner)
            send_line(guest, "/enterlink ask wrong")
            assert "openlink password mismatch" in read_some(guest)
            send_line(owner, "/link_block ask guest")
            assert "blocked guest" in read_some(owner)
            send_line(guest, "/enterlink ask linkpw")
            assert "blocked openlink" in read_some(guest)
            send_line(owner, "/link_unblock ask guest")
            assert "unblocked guest" in read_some(owner)
            send_line(guest, "/enterlink ask linkpw")
            assert "joined ask__guest" in read_some(guest)
            assert "openlink ask new room ask__guest" in read_some(owner)
            send_line(owner, "/linkrooms ask")
            assert "ask__guest" in read_some(owner)
            send_line(owner, "/closelink ask")
            assert "openlink ask closed" in read_some(owner)

            send_line(owner, "/register owner_id pw1")
            assert "registered owner_id" in read_some(owner)
            send_line(owner, "/profile account-profile")
            read_some(owner)
            send_line(owner, "/login owner_id pw1")
            assert "logged in owner_id" in read_some(owner)
            send_line(owner, "/lastseen owner")
            assert "lastseen owner" in read_some(owner)
        finally:
            owner.close()
            member.close()
            guest.close()
    finally:
        stop_server(server)
