#!/usr/bin/env bash
# vms_web_server를 임시 웹 루트로 실행하고 http-server 명세의 시나리오를 실제 요청으로 검사한다.
#
# 사용법: http_server_test.sh <vms_web_server 실행 파일 경로>
# 포트는 HTTP_SERVER_TEST_PORT 환경 변수로 바꿀 수 있다(기본 18080).

set -u

server_binary="$1"
port="${HTTP_SERVER_TEST_PORT:-18080}"
base_url="http://localhost:${port}"

work_dir="$(mktemp -d)"
web_root="${work_dir}/web"
server_log="${work_dir}/server.log"
server_pid=""
failure_count=0

cleanup() {
    if [[ -n "${server_pid}" ]] && kill -0 "${server_pid}" 2>/dev/null; then
        kill -KILL "${server_pid}" 2>/dev/null
    fi
    rm -rf "${work_dir}"
}
trap cleanup EXIT

# 검사 결과를 출력하고 실패 수를 센다.
# $1: 검사 설명, $2: 기대값, $3: 실제값
expect_equal() {
    if [[ "$2" == "$3" ]]; then
        echo "통과: $1"
    else
        echo "실패: $1 (기대: '$2', 실제: '$3')"
        failure_count=$((failure_count + 1))
    fi
}

# curl로 요청하고 상태 코드만 출력한다. 추가 인자는 curl에 그대로 넘긴다.
# $1: 요청 경로
status_of() {
    local path="$1"
    shift
    curl -s -o /dev/null -w '%{http_code}' --path-as-is "$@" "${base_url}${path}"
}

# 응답 헤더 중 하나의 값을 출력한다.
# $1: 요청 경로, $2: 헤더 이름
header_of() {
    curl -s -D - -o /dev/null --path-as-is "${base_url}$1" |
        tr -d '\r' | grep -i "^$2:" | head -n 1 | cut -d ' ' -f 2-
}

# 원본 바이트를 그대로 보내고 응답의 상태 코드만 출력한다.
# 표준 입력으로 보낼 바이트를 받는다.
raw_status() {
    nc -w 5 localhost "${port}" | head -n 1 | cut -d ' ' -f 2
}

mkdir -p "${web_root}/assets"
printf '<p>index</p>' > "${web_root}/index.html"
printf 'console.log("app");' > "${web_root}/app.js"
printf 'binary' > "${web_root}/data.bin"
printf 'secret' > "${web_root}/unreadable.txt"
chmod 000 "${web_root}/unreadable.txt"

"${server_binary}" --port "${port}" --web-root "${web_root}" 2> "${server_log}" &
server_pid=$!

# 서버가 연결을 받을 수 있을 때까지 최대 5초 기다린다.
for _ in $(seq 50); do
    if curl -s -o /dev/null "${base_url}/"; then
        break
    fi
    sleep 0.1
done

# 정적 파일 제공과 응답 헤더
expect_equal "GET / 상태" "200" "$(status_of /)"
expect_equal "GET / 본문" "<p>index</p>" "$(curl -s "${base_url}/")"
expect_equal "GET / Content-Type" "text/html; charset=utf-8" "$(header_of / Content-Type)"
expect_equal "GET / Content-Length" "$(wc -c < "${web_root}/index.html")" "$(header_of / Content-Length)"
expect_equal "GET / Connection" "close" "$(header_of / Connection)"
expect_equal "쿼리 문자열 무시" "200" "$(status_of '/app.js?v=1')"
expect_equal "JS Content-Type" "text/javascript; charset=utf-8" "$(header_of /app.js Content-Type)"
expect_equal "알 수 없는 확장자" "application/octet-stream" "$(header_of /data.bin Content-Type)"

# 요청 오류와 파일 접근 제한
expect_equal "잘못된 요청 줄" "400" "$(printf 'HELLO\r\n\r\n' | raw_status)"
long_header="$(head -c 9000 /dev/zero | tr '\0' 'a')"
expect_equal "헤더 크기 초과" "400" "$(printf 'GET / HTTP/1.1\r\nX-Long: %s' "${long_header}" | raw_status)"
expect_equal "POST는 405" "405" "$(status_of / -X POST)"
expect_equal "405의 Allow 헤더" "GET" \
    "$(curl -s -D - -o /dev/null -X POST "${base_url}/" | tr -d '\r' | grep -i '^Allow:' | cut -d ' ' -f 2-)"
expect_equal "상위 디렉터리 접근" "403" "$(status_of /../etc/passwd)"
expect_equal "없는 파일" "404" "$(status_of /missing.js)"

# root는 권한과 상관없이 파일을 읽을 수 있어 읽기 실패를 만들 수 없으므로 이 검사를 건너뛴다.
if (( EUID == 0 )); then
    echo "건너뜀: 읽기 권한이 없는 파일 (root로 실행 중)"
else
    expect_equal "읽기 권한이 없는 파일" "500" "$(status_of /unreadable.txt)"
fi

# 요청을 끝내지 않은 연결이 있어도 다른 요청은 바로 처리되어야 한다.
# nc만 백그라운드 작업으로 두어야 nc를 종료한 뒤 남은 sleep을 기다리지 않는다.
nc localhost "${port}" < <(printf 'GET / HTTP/1.1\r\n'; sleep 3 2> /dev/null) > /dev/null &
slow_client_pid=$!
sleep 0.2
expect_equal "느린 연결 중 동시 요청" "200" "$(status_of / --max-time 1)"
kill "${slow_client_pid}" 2>/dev/null
wait "${slow_client_pid}" 2>/dev/null

# SIGTERM으로 정상 종료
kill -TERM "${server_pid}"
wait "${server_pid}"
expect_equal "SIGTERM 후 종료 코드" "0" "$?"
server_pid=""

if (( failure_count > 0 )); then
    echo "--- 서버 로그 ---"
    cat "${server_log}"
    echo "실패한 검사: ${failure_count}개"
    exit 1
fi
