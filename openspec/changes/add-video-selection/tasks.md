# Tasks

## 1. JSON 문자열과 UTF-8

- [ ] 1.1 `src/core/json.h/.cpp`에 `IsValidUtf8`과 `AppendJsonString`을 추가하고, `tests/core/json_test.cpp`에서 ASCII·한글·4바이트 문자 통과, 잘린 시퀀스·과잉 표현·서로게이트·U+10FFFF 초과 거부, `"`·`\`·제어 문자(`\u0001`) 이스케이프를 확인한다(`ctest -R json` 통과)

## 2. 쿼리 매개변수

- [ ] 2.1 `src/http/query_string.h/.cpp`에 `FindQueryParameter`를 추가하고, `tests/http/query_string_test.cpp`에서 쿼리 없음, 매개변수 없음, `%EC%8B%9C` 디코딩, `+` → 공백, `=` 없는 쌍, 중복(`kDuplicate`), `%zz`·끝의 `%4`(`kInvalidEncoding`), 다른 이름의 잘못된 인코딩을 확인한다(`ctest -R query_string` 통과)

## 3. HTTP GET 엔드포인트

- [ ] 3.1 `HttpGetHandler`, `HttpGetHandlers`, `HttpEndpoints`를 추가하고 `HttpServer::Start`가 `HttpEndpoints`를 받게 바꾼다. 라우팅을 POST 표 → GET 표 → 정적 파일 순서로 하고 `Allow` 판단에 GET 표를 반영한다. 기존 호출부(`main.cpp`, 테스트)를 고쳐 빌드되는지 확인한다
- [ ] 3.2 GET 엔드포인트 동작을 `tests/http`의 서버 테스트(없으면 새 파일)에서 확인한다: `GET /api/test?x=1`이 핸들러 응답을 받고, `POST /api/test`는 `Allow: GET` 405, 웹 루트에 같은 경로 파일이 있어도 핸들러가 응답한다(`ctest -R http` 통과)

## 4. 동영상 목록

- [ ] 4.1 `src/media/video_library.h/.cpp`에 `VideoLibrary`, `VideoLibraryLimits`, `VideoLibraryLoadResult`와 `LoadDirectory`, `Find(id)`, `Entries()`를 추가한다(design 결정 1·2)
- [ ] 4.2 `tests/media/video_library_test.cpp`에서 임시 디렉터리로 확인한다: 확장자 대소문자·`.264`·`.mp4` 무시·하위 디렉터리 무시, 이름 바이트 순서, 잘못된 파일 경고와 건너뛰기, 없는 대상을 가리키는 심볼릭 링크, UTF-8이 아닌 이름, 작은 `max_total_bytes`로 합계 상한, 빈 목록 오류, 디렉터리가 아님 오류, `Find`의 일치·불일치(`ctest -R video_library` 통과)

## 5. 세션별 동영상

- [ ] 5.1 `IceSessionRequest`에 `video`와 `video_id`를 추가하고, 세션이 이를 보관하게 한다. `IceLiteAgentConfig::video_file`을 지우고 `StartVideoStream`이 세션의 동영상을 쓰며, 송신 시작 로그에 동영상 ID를 넣는다
- [ ] 5.2 `tests/webrtc/ice_lite_agent_test.cpp`의 송신 테스트를 세션별 동영상으로 바꾸고, 동영상 없는 세션은 DTLS 완료 후에도 RTP를 받지 않으며 두 세션에 서로 다른 동영상을 지정하면 각자 자기 첫 프레임을 받는 것을 확인한다(`ctest -R ice_lite_agent` 통과)

## 6. 시그널링

- [ ] 6.1 `Signaling`이 `VideoLibrary`를 참조하게 하고, `HandleOffer`에 `video` 해석(400/404, 원인 로그)을 SDP 검증 앞에 넣어 세션 요청에 동영상을 넘긴다
- [ ] 6.2 `Signaling::HandleVideoList`를 추가해 `{"videos":[{"id":...,"frames":...}]}`를 만든다
- [ ] 6.3 시그널링 테스트(`tests/webrtc`에 없으면 `signaling_test.cpp` 추가)에서 목록에 있는 ID·인코딩된 한글 ID는 200, 없는 ID·`..%2F` 값은 404, `%zz`·중복은 400, `video` 없음은 200이고 동영상 없는 세션, 목록 JSON의 순서·이스케이프·빈 목록을 확인한다(`ctest -R signaling` 통과)

## 7. 명령행과 시작 로그

- [ ] 7.1 `main.cpp`에서 `--video-file`을 `--video-dir`로 바꾸고, `LoadDirectory` 결과의 경고와 동영상별 ID·프레임 수, 동영상 수와 fps를 로그에 남긴다. `/api/videos`를 GET 엔드포인트로 등록한다. 사용법 문구를 고친다
- [ ] 7.2 `tests/integration/http_server_test.sh`를 고친다: `--video-file`은 시작 실패, 없는 `--video-dir`·일반 파일 `--video-dir`·쓸 수 있는 파일이 없는 디렉터리는 시작 실패, `--video-dir` 없이 `GET /api/videos`는 `{"videos":[]}`, `POST /api/videos`는 `Allow: GET` 405, `POST /offer?video=missing.h264`는 404(`ctest -R integration` 통과)

## 8. 웹 페이지와 문서

- [ ] 8.1 `web/index.html`에 동영상을 고르는 `select` 요소를 두고, `web/app.js`가 목록을 채우고 선택한 ID를 `video` 쿼리로 붙여 연결하게 한다. 목록이 비었거나 실패하면 문구를 표시하고 `select` 요소를 비활성화한다
- [ ] 8.2 README의 소개, 인자 표, 실행 예, 확인 절차를 `--video-dir` 기준으로 고친다. 테스트 영상 두 개(예: `testsrc2`와 `smptebars`)를 만드는 ffmpeg 명령과 "같은 fps의 파일만 둔다"는 주의를 적고, 적은 명령을 그대로 실행해 서버가 두 동영상으로 시작하는지 확인한다

## 9. 통합 검증

- [ ] 9.1 Windows Chrome에서 확인한다: `select` 요소에 두 동영상이 이름순으로 나옴, 각 동영상을 골라 연결하면 그 영상이 재생됨, 재생 중 다른 영상으로 다시 연결하면 새 영상이 처음부터 재생됨, 두 탭이 서로 다른 영상을 동시에 재생하고 서버 로그에 세션별 동영상 ID가 나옴
- [ ] 9.2 `--video-dir` 없이 실행해 페이지에 "동영상 없음"이 표시되고 연결 상태가 `connected`가 되며 영상이 나오지 않는지 확인한다
