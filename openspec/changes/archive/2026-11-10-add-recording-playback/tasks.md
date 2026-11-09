# Tasks

용어와 식별자의 뜻은 [background.md](background.md)에 있다.

## 1. 녹화: `RecordedFile::fragments`와 `RecordingCatalogEvent`

- [x] 1.1 [`RecordedFile`](background.md#recordedfile)에 `fragments`를 더한다(design 2절).
  1. `src/recording/recorded_file.h`에 구조체 [`RecordedFragment`](background.md#recordedfragment)를 더한다. 멤버는 `start_time`, `duration`, `offset`, `size`다.
  2. `src/recording/recorded_file.h`의 `RecordedFile`에 멤버 `fragments`(`std::vector<RecordedFragment>`)를 더한다.
  3. [`RecoverRecordings()`](background.md#recoverrecordings)를 고친다. 고친 `RecoverRecordings()`는 CRC가 맞는 [`RecordingIndexRecord`](background.md#recordingindexrecord)마다 다음을 한다.
     - 레코드에서 `fragment_crc32`를 뺀 `RecordedFragment`를 만든다.
     - 만든 `RecordedFragment`를 그 파일의 `RecordedFile::fragments`에 더한다.
  4. `tests/recording/recording_recovery_test.cpp`에 시험을 더한다. 시험은 두 가지를 확인한다.
     - 정상 파일의 `fragments`는 `.idx`의 `RecordingIndexRecord`와 같다.
     - `.mp4`를 세 번째 fragment 중간에서 잘라 낸 파일의 `fragments`는 앞의 레코드 2개만 가진다.
  5. `ctest --test-dir build -R recording_recovery`가 통과하는지 확인한다.
- [x] 1.2 녹화 스레드가 [`RecordingCatalogEvent`](background.md#recordingcatalogevent)를 알리게 한다(design 3절).
  1. `src/recording/recording_catalog_event.h`에 `RecordedFileClosed`, `RecordedFileDeleted`, `RecordingCatalogEvent`, [`RecordingCatalogListener`](background.md#recordingcataloglistener)를 둔다.
  2. [`RecordingWriter`](background.md#recordingwriter)의 생성자에 `RecordingCatalogListener` 인자를 더한다. `RecordingWriter`는 받은 함수를 `RecordingFileStore`의 생성자에 넘긴다.
  3. [`RecordingFileStore`](background.md#recordingfilestore)의 생성자에 `RecordingCatalogListener` 인자를 더한다. `RecordingFileStore`는 받은 함수를 멤버 `listener_`에 둔다.
  4. `RecordingFileStore::WriteFragment()`를 고친다. 고친 `WriteFragment()`는 fragment 바이트와 `RecordingIndexRecord`를 파일에 쓴 뒤, 그 fragment의 `RecordedFragment`를 `OpenRecording::fragments`에 더한다.
  5. `RecordingFileStore::FinishFile()`을 고친다. 고친 `FinishFile()`은 다음 순서로 동작한다.
     - 녹화 파일을 동기화한다.
     - 녹화 파일을 닫는다.
     - `fragments`를 비운 `RecordedFile` 사본을 `closed_files_`에 더한다.
     - `fragments`를 채운 `RecordedFile`을 `RecordedFileClosed`에 담는다.
     - 그 `RecordedFileClosed`를 `listener_`로 알린다.
  6. `RecordingFileStore::ApplyRetention()`과 `RecordingFileStore::DeleteOldestSegment()`를 고친다. 두 함수는 `.mp4`와 `.idx`를 지울 때마다 `RecordedFileDeleted`를 `listener_`로 알린다. `RecordedFileDeleted`는 지운 파일의 `camera_folder`와 `stem`을 담는다.
  7. `tests/recording/recording_file_store_test.cpp`와 `tests/recording/recording_writer_test.cpp`에 시험을 더한다. 시험은 받은 이벤트를 벡터에 모으는 `RecordingCatalogListener`를 생성자에 넘긴다. 시험은 모은 이벤트로 네 가지를 확인한다.
     - `RecordedFileClosed`의 `fragments`는 디스크의 `.idx`에 있는 `RecordingIndexRecord`와 같다.
     - `ApplyRetention()`이 파일을 지울 때마다 `RecordedFileDeleted`가 하나 온다.
     - `DeleteOldestSegment()`가 파일을 지울 때마다 `RecordedFileDeleted`가 하나 온다.
     - `FinishFile()`이 fragment가 없는 파일을 지울 때는 이벤트가 오지 않는다.
  8. `ctest --test-dir build -R recording`이 통과하는지 확인한다.

## 2. 재생 모듈

- [x] 2.1 [`EncodeQueryComponent()`](background.md#findqueryparameter와-encodequerycomponent)를 만든다(design 6절).
  1. `src/http/query_string.h`와 `src/http/query_string.cpp`에 `EncodeQueryComponent()`를 더한다. 이 함수는 UTF-8 바이트 중 영문자, 숫자, `-._~`를 그대로 둔다. 이 함수는 나머지 바이트를 대문자 `%XX`로 바꾼다.
  2. `tests/http/query_string_test.cpp`에 시험을 더한다. 시험은 두 가지를 확인한다.
     - `정문 1`을 인코딩한 결과는 `%EC%A0%95%EB%AC%B8%201`이다.
     - 인코딩한 값을 `FindQueryParameter()`로 디코딩한 결과는 원래 값과 같다.
  3. `ctest --test-dir build -R query_string`이 통과하는지 확인한다.
- [x] 2.2 [`RecordingLibrary`](background.md#recordinglibrary)를 만든다(design 5절).
  1. `src/playback/recording_library.h`와 `src/playback/recording_library.cpp`에 `RecordingLibrary`를 만든다.
  2. 루트 `CMakeLists.txt`에 `playback` 정적 라이브러리를 더한다.
  3. `tests/CMakeLists.txt`에 `tests/playback/` 하위 디렉터리를 더한다.
  4. `tests/playback/recording_library_test.cpp`에 시험을 더한다. 시험은 다음을 확인한다.
     - `RecordedFileCatalog`로 `RecordingLibrary`를 만들면, 이름이 UTF-8이 아닌 폴더가 빠진다.
     - `RecordedFileCatalog`로 `RecordingLibrary`를 만들면, 파일이 없는 폴더가 빠진다.
     - `Apply()`에 `RecordedFileClosed`를 넘기면, 그 파일이 폴더의 파일 목록에서 `start_unix_ms` 순서 자리에 들어간다.
     - `Apply()`에 `RecordedFileDeleted`를 넘기면, 그 파일이 폴더의 파일 목록에서 빠진다.
     - `Apply()`가 폴더의 마지막 파일을 빼면, 그 폴더가 `RecordingLibrary`에서 지워진다.
     - `FindFolder()`는 `camera_folder`로 파일 목록을 찾는다.
     - `FindFile()`은 `camera_folder`와 `stem`으로 `RecordedFile`을 찾는다.
  5. `ctest --test-dir build -R recording_library`가 통과하는지 확인한다.
- [x] 2.3 재생 목록 생성 함수를 만든다(design 6절, 7절).
  1. `src/playback/hls_playlist.h`와 `src/playback/hls_playlist.cpp`에 [`BuildRecordingFileUrl()`](background.md#buildrecordingfileurl)을 만든다.
  2. 같은 파일에 [`BuildHlsPlaylist()`](background.md#buildhlsplaylist)를 만든다.
  3. `tests/playback/hls_playlist_test.cpp`에 시험을 더한다. 시험은 specs `recording-playback`의 다음 시나리오를 확인한다.
     - "재생 목록 머리": `#EXT-X-TARGETDURATION:1`과 `#EXT-X-TARGETDURATION:2`
     - "재생 목록의 파일 구간": 파일 두 개 사이의 `#EXT-X-DISCONTINUITY`
     - "재생 목록의 세그먼트": `duration`이 89910인 fragment의 `#EXTINF:0.999,`
     - "녹화 파일 URL": 한글과 공백이 든 `camera_folder`를 인코딩한 URL
  4. `ctest --test-dir build -R hls_playlist`가 통과하는지 확인한다.
- [x] 2.4 [`RecordingPlaybackEndpoints`](background.md#recordingplaybackendpoints)를 만든다(design 5절, 6절).
  1. `src/playback/recording_playback_endpoints.h`와 `src/playback/recording_playback_endpoints.cpp`에 `RecordingPlaybackEndpoints`를 만든다.
  2. `HandlePlaylist()`는 `GET /api/recordings/playlist.m3u8` 요청을 처리한다.
  3. `HandleFile()`은 `GET /api/recordings/file.mp4` 요청을 처리한다.
  4. `tests/playback/recording_playback_endpoints_test.cpp`에 시험을 더한다. 시험은 다음을 확인한다.
     - `HandlePlaylist()`는 있는 폴더의 요청에 `200`과 `Content-Type: application/vnd.apple.mpegurl`로 응답한다.
     - `HandleFile()`은 있는 파일의 요청에 `HttpFileResponse`를 돌려준다. 그 응답의 경로는 `<녹화 디렉터리>/<camera_folder>/<stem>.mp4`이고, `Content-Type`은 `video/mp4`다.
     - 두 핸들러는 매개변수가 없는 요청에 `400`으로 응답한다.
     - 두 핸들러는 같은 매개변수가 두 번 오는 요청에 `400`으로 응답한다.
     - 두 핸들러는 퍼센트 인코딩이 잘못된 요청에 `400`으로 응답한다.
     - 두 핸들러는 `RecordingLibrary`에 없는 폴더의 요청에 `404`로 응답한다.
     - `HandleFile()`은 `RecordingLibrary`에 없는 파일의 요청에 `404`로 응답한다.
     - 두 핸들러는 `..`이 든 값의 요청에 `404`로 응답한다.
  5. `ctest --test-dir build -R recording_playback_endpoints`가 통과하는지 확인한다.
- [x] 2.5 소스 목록 생성을 `src/playback/`으로 옮긴다(design 5절).
  1. `src/playback/source_list.h`와 `src/playback/source_list.cpp`에 [`BuildSourceListResponse()`](background.md#buildsourcelistresponse)를 만든다.
  2. `src/webrtc/signaling.h`와 `src/webrtc/signaling.cpp`에서 `Signaling::HandleSourceList()`를 지운다.
  3. `tests/webrtc/signaling_test.cpp`의 소스 목록 시험을 `tests/playback/source_list_test.cpp`로 옮긴다.
  4. `tests/playback/source_list_test.cpp`에 시험을 더한다. 시험은 specs `video-catalog`의 "녹화가 있는 폴더" 시나리오와 "녹화 폴더 이름" 시나리오를 확인한다.
     - `recording` 원소는 `camera` 원소 뒤에 온다.
     - `recording` 원소끼리는 `camera_folder` 순서로 정렬된다.
     - 파일이 없는 폴더는 응답에 없다.
     - 따옴표가 든 한글 `camera_folder`는 JSON 문자열로 이스케이프된다.
  5. `ctest --test-dir build -R "source_list|signaling"`이 통과하는지 확인한다.

## 3. 서버 연결

- [x] 3.1 `src/apps/web_server/main.cpp`를 고친다(design 4절).
  1. `RecoverRecordings()`의 결과로 `RecordingLibrary`를 만든다.
  2. `EventLoop`를 `RecordingWriter`보다 먼저 선언한다.
  3. `RecordingWriter`에 `RecordingCatalogListener`를 넘긴다. 이 함수는 받은 이벤트를 [`EventLoop::Post()`](background.md#eventlooppost)로 이벤트 루프 스레드에 넘긴다. 이벤트 루프 스레드는 넘겨받은 작업에서 `RecordingLibrary::Apply()`를 부른다.
  4. `/api/sources` 핸들러를 고친다. 고친 핸들러는 `Signaling::HandleSourceList()` 대신 `BuildSourceListResponse()`를 부른다.
  5. HTTP 서버의 GET 엔드포인트 표에 `/api/recordings/playlist.m3u8`을 등록한다.
  6. HTTP 서버의 GET 엔드포인트 표에 `/api/recordings/file.mp4`를 등록한다.
  7. 선언 순서를 설명하는 주석을 바뀐 순서에 맞게 고친다.
  8. 빌드가 되는지 확인한다.
  9. `ctest --test-dir build`가 모두 통과하는지 확인한다.
- [x] 3.2 통합 시험을 더한다.
  1. `tests/integration/make_test_recording.cpp`에 테스트 도구를 만든다. 이 도구는 `RecordingFileStore`로 합성 프레임을 쓴다. 그 결과 인자로 받은 녹화 디렉터리의 카메라 폴더에 닫힌 `.mp4`와 `.idx`가 하나씩 생긴다.
  2. `tests/integration/http_server_test.sh`에 시나리오를 더한다. 이 시나리오는 테스트 도구로 만든 녹화 디렉터리로 서버를 실행한다. 시나리오는 다음을 확인한다.
     - `/api/sources` 응답에 `recording` 원소가 있다.
     - 서버는 재생 목록 요청에 `200`과 `Content-Type: application/vnd.apple.mpegurl`로 응답한다.
     - 시나리오는 재생 목록의 첫 `#EXT-X-BYTERANGE` 값을 `Range` 헤더로 바꿔 녹화 파일을 요청한다. 서버는 이 요청에 `206`으로 응답한다. 응답 본문의 크기는 그 `#EXT-X-BYTERANGE`의 크기와 같다.
     - 서버는 없는 파일의 녹화 파일 요청에 `404`로 응답한다.
     - 서버는 `camera`가 없는 재생 목록 요청에 `400`으로 응답한다.
     - 서버는 `type=recording`인 `POST /offer`에 `400`으로 응답한다.
  3. `ctest --test-dir build -R http_server`가 통과하는지 확인한다.

## 4. 웹 페이지와 문서

- [x] 4.1 [hls.js](background.md#hlsjs) 배포 파일을 넣는다(design 8절).
  1. 작업 시점의 hls.js 1.x 안정 버전 하나를 고른다.
  2. 그 버전의 `hls.min.js`와 `LICENSE`를 `web/vendor/hls.js-<버전>/`에 넣는다.
  3. 서버를 실행한다.
  4. `curl -I http://localhost:8080/vendor/hls.js-<버전>/hls.min.js`를 실행한다. 서버가 `200`과 `text/javascript` 계열의 `Content-Type`으로 응답하는지 확인한다.
- [x] 4.2 웹 페이지에 녹화 재생을 더한다(design 8절).
  1. `web/index.html`에 hls.js를 불러오는 `<script>`를 더한다.
  2. `web/index.html`에 "녹화 재생" 표시 항목과 "녹화 시각" 표시 항목을 더한다.
  3. `web/app.js`의 종류 이름 표에 `recording`의 이름 "녹화"를 더한다.
  4. `web/app.js`의 연결 버튼 처리를 고친다. 고친 처리는 새 재생을 시작하기 전에 `RTCPeerConnection`과 `Hls` 객체를 모두 정리한다.
  5. `web/app.js`에 녹화 재생을 더한다. 고른 소스의 `type`이 `recording`이면 `app.js`는 `Hls` 객체로 재생 목록을 `<video>` 요소에 재생한다.
  6. 녹화를 재생하는 동안 `app.js`는 `<video>` 요소의 `controls`를 켠다. WebRTC로 재생할 때 `app.js`는 `controls`를 끈다.
  7. `<video>` 요소의 `timeupdate` 이벤트마다 `app.js`는 `hls.playingDate`를 읽는다. `app.js`는 읽은 값을 "녹화 시각"에 표시한다.
  8. `Hls.Events.ERROR`의 `fatal`이 참이면 `app.js`는 "녹화 재생"에 오류를 표시한다. 그다음 `app.js`는 재생을 멈춘다.
  9. Windows Chrome에서 "녹화 · <camera_folder>"를 고른다. 다음을 확인한다.
     - 영상이 재생된다.
     - 재생 막대로 재생 위치를 옮길 수 있다.
     - "녹화 시각"이 표시된다.
  10. 그다음 "카메라"나 "파일" 소스를 고른다. 영상이 WebRTC로 재생되는지 확인한다. 재생 막대가 사라지는지 확인한다.
- [x] 4.3 `README.md`에 녹화 재생 절을 더한다.
  1. 녹화 재생 절에 다음 내용을 적는다.
     - 웹 페이지에서 "녹화 · <camera_folder>"를 고르는 방법
     - 서버는 닫힌 파일만 재생한다. 그래서 지금 녹화 중인 파일(최근 최대 `--record-segment`초)은 재생되지 않는다.
     - 카메라가 꺼졌던 구간은 재생 위치가 건너뛴다.
     - 재생하는 동안 `--record-retention`을 넘은 파일이 지워지면 재생이 멈춘다.
     - `--record-retention`이 길수록 재생 목록이 커진다.
     - 재생 목록 URL과 녹화 파일 URL
     - 두 URL을 `curl`로 확인하는 예
  2. 같은 절에 hls.js 버전과 라이선스를 적는다.
  3. README에 적은 `curl` 명령을 실제로 실행한다. 출력이 README 설명과 같은지 확인한다.

## 5. 통합 확인

- [x] 5.1 빈 빌드 디렉터리에서 `cmake -B build && cmake --build build -j && ctest --test-dir build --output-on-failure`를 실행한다. 모든 테스트가 통과하는지 확인한다.
- [x] 5.2 카메라가 꺼졌던 구간을 건너뛰는지 확인한다.
  1. vms-cam의 `vms_camera_server`를 카메라로 실행한다.
  2. 서버를 `--camera cam=...`과 `--record-segment 10`으로 실행한다.
  3. 녹화 스레드가 첫 파일을 닫을 때까지 기다린다.
  4. 웹 페이지를 새로 연다. 소스 목록에 "녹화 · cam"이 있는지 확인한다. "녹화 · cam"이 재생되는지 확인한다.
  5. `vms_camera_server`를 30초 동안 끈다. 그다음 `vms_camera_server`를 다시 켠다.
  6. 녹화 스레드가 다음 파일을 닫을 때까지 기다린다.
  7. "녹화 · cam"에 다시 연결한다. 재생 위치가 꺼졌던 구간을 건너뛰는지 확인한다. "녹화 시각"이 다음 파일의 시작 시각으로 바뀌는지 확인한다.
- [x] 5.3 재생 중에 지워진 파일의 처리를 확인한다.
  1. 서버를 `--record-retention 30 --record-segment 10`으로 실행해 녹화한다.
  2. 웹 페이지에서 녹화를 재생한다.
  3. 재생하는 상태로 기다린다.
  4. `ApplyRetention()`이 지운 파일의 세그먼트를 hls.js가 요청할 때를 본다. 웹 페이지가 "녹화 재생"에 오류를 표시하는지 확인한다. 웹 페이지가 재생을 멈추는지 확인한다.

## Workflow follow-up

- 구현과 확인을 마치면 change를 아카이브하도록 사용자에게 제안한다.
- 아카이브 이동과 명세 반영을 커밋하도록 사용자에게 제안한다.
- 아카이브한 뒤 `openspec/specs/video-catalog/spec.md`의 Purpose 문장을 고친다. 지금 문장("이후 카메라와 녹화 파일도 같은 목록으로 제공된다")을 현재 상태에 맞게 바꾼다.
