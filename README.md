# chatting-client

## 프로그램 목적과 구조

이 프로그램은 TCP 소켓과 POSIX 스레드를 이용하여 여러 사용자가 동시에 접속할 수 있는 채팅 서버와 클라이언트를 구현합니다. 서버는 `multi_server.cc`에서 클라이언트 접속마다 `pthread_create`로 독립 처리 스레드를 만들고, 클라이언트는 `multi_client.cc`에서 송신 루프와 수신 스레드를 분리하여 메시지를 주고받습니다.

클라이언트는 표준 입력으로 명령어와 메시지를 전송합니다. 서버는 방, 사용자, 메시지, 계정, 오픈링크 상태를 메모리 구조에 보관하고 일부 설정은 상태 파일에 저장합니다.

## 핵심 기능과 구현 방식

### 접속과 기본 채팅

서버는 `clients`, `rooms`, `messages` 전역 컨테이너로 접속자, 방, 메시지 기록을 관리합니다. `handle_clnt` 함수는 클라이언트별 수신 루프를 담당하고, `handle_line` 함수는 일반 메시지와 슬래시 명령을 구분합니다. 일반 메시지는 `send_chat_text` 함수에서 권한을 확인한 뒤 `MessageRecord`로 저장하고 방 참여자에게 전파합니다.

사용자 편의를 위하여 화면에 표시되는 메시지에는 내부 메시지 번호 대신 현재 시각을 표시합니다. 메시지 번호가 필요한 기능은 `/history`에서 확인하도록 설계했습니다.

### 계정, 프로필, 접속 상태

`Account` 구조체와 `accounts` 맵은 `/register`, `/login` 기능을 구현합니다. 로그인 시 닉네임, 프로필, 상태 공개 설정을 복원합니다. `Client` 구조체는 `profile`, `status`, `profile_public`, `status_public`, `last_active` 필드를 포함합니다.

사용자 편의를 위하여 프로필 공개와 접속 상태 공개를 분리했습니다. `visible_profile_for`, `visible_status_for`, `handle_lastseen` 함수는 상대가 볼 수 있는 정보만 출력합니다.

### 방 생성과 방 관리

`Room` 구조체는 방 이름, 방 타입, 방장, 관리자, 발언 허가자, 차단자, 발언 요청자 목록을 저장합니다. `create_room_command`, `handle_join`, `handle_rename_room`, `handle_delete_room`, `handle_set_room_type`, `handle_transfer_owner` 함수가 방 생성, 입장, 이름 변경, 삭제, 타입 변경, 방장 이전을 담당합니다.

방 삭제 시에는 모든 접속자를 `lobby`로 이동시키고 `rooms`, `private_room_links`, `mod_logs`, `messages`, `message_order`, `reactions`, 사용자별 `muted_rooms`에서 삭제된 방 참조를 제거합니다. 이로써 방 목록, 사용자 입장 상태, 메시지 기록, 고정 메시지, 반응 조회가 삭제된 방을 참조하지 않도록 했습니다.

### 승인방, 공지방, 숨김방, 비밀방

방 타입은 `RoomType` 열거형으로 구분합니다. 승인방은 `ROOM_APPROVAL`, 공지방은 `ROOM_ANNOUNCE`, 숨김방은 `ROOM_HIDDEN`, 비밀방은 `ROOM_SECRET`으로 관리합니다. 발언 가능 여부는 `can_speak_unlocked` 함수에서 판단합니다.

사용자 편의를 위하여 승인방에는 `/request_speak`, `/requests`, `/approve`, `/deny` 흐름을 추가했습니다. 사용자는 직접 발언 권한을 요청할 수 있고, 관리자는 요청 목록을 확인하여 승인하거나 거절할 수 있습니다. 거절 사유 입력 기능은 포함하지 않았습니다.

숨김방은 `Room::anonymous_names`와 `display_name_unlocked` 함수로 같은 사용자가 같은 익명 번호를 유지하도록 했습니다. 비밀방은 암호화 기능이 아니라 비밀번호 입장 제한 기능이므로 명령 이름을 `/create_secret`, `/join_secret`으로 표시합니다.

### 관리자 권한, 강퇴, 차단

관리 권한은 `Room::owner`, `Room::admins`, `is_admin_unlocked`, `is_owner_unlocked`로 구분합니다. `/delegate`, `/undelegate`, `/transfer_owner`는 관리자와 방장 권한을 관리합니다. `/kick`, `/ban`, `/unban`, `/banlist`는 강퇴와 재입장 차단을 처리합니다.

사용자 편의를 위하여 `/kick`은 즉시 `lobby`로 이동시키고, `/ban`은 이동과 동시에 해당 방 재입장을 막도록 했습니다. 관리 작업은 `add_mod_log_unlocked`와 `mod_logs`에 기록됩니다.

### 메시지 기록, 검색, 답장, 수정, 삭제

`MessageRecord` 구조체는 메시지 ID, 방, 작성자, 표시 이름, 내용, 시간, 답장 대상, 수정 여부, 삭제 여부, 고정 여부를 저장합니다. `/history`와 `/recent`는 최근 메시지를 ID와 함께 보여주며, `/search`는 키워드 검색을, `/since`는 특정 메시지 ID 이후의 메시지 조회를 수행합니다.

사용자 편의를 위하여 `/reply`, `/edit`, `/delete`, `/react`, `/pin`처럼 메시지 ID가 필요한 기능은 `/history`를 기준으로 사용할 수 있게 했습니다.

### 메시지 고정과 이모지 반응

`MessageRecord::pinned`, `MessageRecord::pinned_by`, `reactions` 맵으로 고정 메시지와 이모지 반응을 관리합니다. `/pin`, `/unpin`, `/pinned`, `/react`, `/reactions` 명령을 제공합니다.

공지방을 제외한 방에서는 누구나 메시지를 고정할 수 있습니다. 공지방에서는 관리자만 고정할 수 있습니다. 고정 해제는 고정한 사람 또는 방장만 할 수 있습니다.

### 오픈링크 1대1 방

`open_links`, `private_room_links`, `open_link_passwords`, `open_link_banned` 컨테이너는 오픈링크와 링크로 생성된 1대1 방을 관리합니다. `/openlink`, `/enterlink`, `/closelink`, `/linkrooms`, `/close_private`, `/set_link_password`, `/clear_link_password`, `/link_block`, `/link_unblock` 명령을 제공합니다.

사용자 편의를 위하여 오픈링크 소유자는 링크별 1대1 방 목록을 확인하고, 링크 비밀번호와 차단 목록을 설정할 수 있습니다. 이를 통해 상담형 1대다 흐름에서 여러 개의 1대1 대화를 관리할 수 있습니다.

### 멘션, 알림, 뮤트

`notify_mentions`, `handle_mentions`, `handle_unread`, `handle_mute`, `handle_unmute` 함수는 `@닉네임` 멘션, 개인 알림 목록, 방별 알림 뮤트를 처리합니다. `Client::mentions`와 `Client::muted_rooms`에 사용자별 알림 상태를 저장합니다.

사용자 편의를 위하여 방이 많아져도 자신이 언급된 메시지를 따로 확인할 수 있도록 `/mentions`와 `/unread`를 제공합니다.

### 템플릿 자동완성

`templates` 맵과 `handle_template_command` 함수는 자주 쓰는 문장 템플릿을 관리합니다. `/template add`, `/template del`, `/template list`, `/template suggest`, `/template use` 명령을 제공합니다.

사용자 편의를 위하여 `네, 알겠습니다`, `안녕하세요!` 같은 반복 문장을 키워드로 저장하고 빠르게 전송할 수 있게 했습니다.

## 테스트

`chat_common_test.cc`는 공통 문자열 유틸리티를 검증합니다. `chat_final_integration_test.py`는 서버를 실제 프로세스로 실행하고 여러 클라이언트 소켓을 연결하여 메시지 송수신, 프로필 공개 설정, 방 나가기, 강퇴, 히스토리, 답장, 고정, 승인방, 차단, 방 삭제 정합성, 비밀방, 오픈링크, 계정 기능을 통합 검증합니다.
