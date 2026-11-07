# Tasks

## 1. 파일 이름과 단위 구간

- [x] 1.1 `src/recording/recording_file_name.h/.cpp`에 시작 시각(유닉스 밀리초) ↔ `YYYYMMDDTHHMMSSZ[-n].mp4`·`.idx` 이름 변환과 단위 구간 번호 계산을 추가한다(design 결정 1·4). `CMakeLists.txt`에 `src/recording/*.cpp`, `tests/CMakeLists.txt`에 `tests/recording`을 추가한다. `tests/recording/recording_file_name_test.cpp`에서 확인한다(`ctest -R recording_file_name` 통과)
  - 시각 → 이름 → 시각 왕복(초 단위로 내림)
  - `-1`, `-2` 접미사 해석과 생성
  - 규칙에 맞지 않는 이름 거부
  - 단위 구간 번호: 경계 직전·직후, 단위 60과 3600

## 2. CRC32와 색인 파일

- [x] 2.1 `src/recording/recording_index.h/.cpp`에 CRC32(0xEDB88320 표 계산), `.idx` 헤더·레코드 인코딩과 디코딩을 추가한다(design 결정 6)
- [x] 2.2 `tests/recording/recording_index_test.cpp`에서 확인한다(`ctest -R recording_index` 통과)
  - `"123456789"`의 CRC32가 `0xCBF43926`
  - 헤더·레코드 왕복
  - 바이트 하나를 바꾼 헤더·레코드의 CRC 실패
  - 모자란 바이트(잘린 레코드)는 디코딩 실패

## 3. fMP4 작성

- [x] 3.1 `src/recording/fmp4.h/.cpp`에 SPS·PPS로 초기화 구간(`ftyp`+`moov`)을, 프레임 목록(NAL 바이트, 재생 시각, 길이)과 조각 번호로 조각(`moof`+`mdat`)을 만드는 함수를 추가한다(design 결정 5). 해상도는 `ParseH264SpsResolution`, 프로파일·레벨은 SPS 앞 3바이트에서 읽는다
- [x] 3.2 `tests/recording/fmp4_test.cpp`에서 테스트 안의 상자 해석기로 확인한다(`ctest -R fmp4` 통과)
  - 초기화 구간의 상자 순서와 크기, `mdhd` timescale 90000, `tkhd` 해상도, `avcC`의 프로파일·레벨·SPS·PPS
  - 조각의 `mfhd` 번호, `tfdt` 값, `trun` 프레임 수·길이·크기, 첫 프레임만 sync 플래그
  - `trun`의 `data-offset`이 `mdat` 데이터 시작을 가리키고, 그 뒤가 4바이트 길이 + NAL 순서
  - AUD·SPS·PPS NAL을 넣지 않음
- [x] 3.3 3.1의 함수로 테스트 프레임 몇 개를 담은 파일을 만들고, `ffprobe`가 있으면 H.264 스트림과 해상도를 읽는지 확인한다. 확인 결과를 작업 메모로 남긴다. 자동 테스트에는 넣지 않는다
  - 작업 메모: ffmpeg(libx264, Constrained Baseline 320x240, 30fps, GOP 30)로 만든 2초 Annex-B 파일을 `H264File`로 읽어 `BuildFmp4InitSegment`·`BuildFmp4Fragment`로 fMP4를 만들었다. `ffprobe -show_entries stream=codec_name,profile,width,height,duration`이 `h264`, `Constrained Baseline`, `320x240`, 길이 2.000000을 보여 주었고, `ffmpeg -i test.mp4 -f null -`로 오류 없이 디코딩되었다.

## 4. 카메라 녹화 상태

- [x] 4.1 `src/recording/recording_options.h`에 녹화 설정 구조체를, `src/recording/recording_task.h`에 파일 작업과 작업 대상 인터페이스(`Push`, `HasRoom`)를, `src/recording/camera_recording_planner.h/.cpp`에 design 결정 4의 상태 기계를 추가한다. 계획기는 만든 파일 작업을 생성자에서 받은 함수로 넘긴다
- [x] 4.2 `tests/recording/camera_recording_planner_test.cpp`에서 수신 시각을 담은 입력으로 확인한다(`ctest -R camera_recording_planner` 통과)
  - 첫 키프레임 전 프레임은 무시하고, 첫 키프레임에서 `OpenFile`(수신 시각, SPS, PPS)
  - 다음 키프레임에서 앞 GOP의 `WriteFragment`. 프레임 재생 시각과 길이가 RTP 차이와 같고, AUD·SPS·PPS 제외
  - 단위 구간이 바뀐 뒤 키프레임에서 `CloseFile`+`OpenFile`
  - 손실: 파일을 닫지 않고 다음 키프레임까지 버림. 다음 키프레임에서 앞부분 GOP의 마지막 프레임 길이가 그 키프레임까지
  - 큐 넘침: 손실과 같이 처리
  - 끊김: 모으던 GOP를 쓰고 `CloseFile`. 마지막 프레임 길이는 직전 간격
  - 버리는 중 끊김: 붙잡은 GOP를 직전 간격으로 쓰고 `CloseFile`
  - 시계가 뒤로 가면 새 파일 시작 시각이 직전 파일 끝 시각
  - RTP 타임스탬프 순환 구간에서도 재생 시각이 늘어남
- [x] 4.3 `src/recording/camera_recorder.h/.cpp`에 소스 구독, NAL 복사와 수신 시각 기록, 키프레임에서의 `HasRoom` 확인, 계획기 호출을 추가한다. 작업 대상과 현재 시각 함수를 인자로 받는다. `tests/recording/camera_recorder_test.cpp`에서 받은 시각으로 파일을 열고, 여유 없는 키프레임부터 GOP를 버려 앞 프레임을 늘리며, 손실·끊김을 넘기고 연결은 무시하는지 확인한다(`ctest -R camera_recorder` 통과)

## 5. 녹화 스레드

- [x] 5.1 `src/recording/recording_file_store.h/.cpp`에 파일 작업 실행(파일 열기·쓰기·닫기, 이름 충돌 번호, 동기화)과 design 결정 8의 보관 분량 삭제·비상 삭제, `ENOSPC` 처리를 구현한다. 여유 공간 조회 함수는 설정으로 받는다
- [x] 5.2 `tests/recording/recording_file_store_test.cpp`에서 임시 디렉터리로 확인한다(`ctest -R recording_file_store` 통과)
  - 작업 순서대로 `.mp4`·`.idx`가 생기고, 색인 레코드가 가리키는 바이트가 `moof`로 시작하며 CRC32가 맞음
  - 같은 이름의 파일이 있으면 `-1`을 붙여 새로 만듦
  - 보관 분량을 넘으면 가장 오래된 닫힌 파일 짝이 지워지고, 끊긴 시간은 분량에 들지 않음
  - 가짜 여유 공간이 부족하면 모든 카메라 폴더(등록되지 않은 폴더 포함)에서 가장 오래된 단위 구간 파일을 함께 지움
  - 열린 파일은 지우지 않음
- [x] 5.3 `src/recording/recording_writer.h/.cpp`에 작업 큐(바이트 합과 `HasRoom`), 녹화 스레드, 파일 저장소 실행, 동기화 주기, 종료 시 큐 비우기를 구현한다(design 결정 2·3). `CMakeLists.txt`에 스레드 라이브러리(`Threads::Threads`)를 연결한다
- [x] 5.4 `tests/recording/recording_writer_test.cpp`에서 임시 디렉터리로 확인한다(`ctest -R recording_writer` 통과)
  - 작업 순서대로 파일 짝이 생김
  - 바이트 합이 상한 이상이면 `HasRoom()`이 false이고, 쓰기를 마치면 다시 true. 상한을 넘긴 조각도 거부하지 않음
  - 소멸할 때 큐에 남은 작업을 모두 쓰고 파일을 닫음
- [x] 5.5 `-fsanitize=thread`로 `recording_writer_test`를 한 번 빌드·실행해 경고가 없는지 확인하고, 실행한 명령을 작업 메모로 남긴다
  - 작업 메모: `cmake -S . -B <tsan 빌드 디렉터리> -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS=-fsanitize=thread -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=thread`, `cmake --build <tsan 빌드 디렉터리> --target recording_writer_test`로 빌드해 `<tsan 빌드 디렉터리>/tests/recording/recording_writer_test`를 5번 실행했다. `HasRoom` 시험(녹화 스레드가 막힌 동안 다른 스레드에서 넣고 확인하기)을 포함해 ThreadSanitizer 경고 없이 종료 코드 0이었다. 이전 구조에서는 종료 시 열린 파일을 닫는 반복문의 heap-use-after-free를 찾아 고쳤다.

## 6. 시작할 때 복구

- [x] 6.1 `src/recording/recording_recovery.h/.cpp`에 design 결정 9의 복구를 구현한다. 결과는 카메라 폴더별 닫힌 파일 목록이며 `RecordingWriter`의 초기 목록이 된다
- [x] 6.2 `tests/recording/recording_recovery_test.cpp`에서 5.1로 만든 정상 파일을 손상시켜 확인한다(`ctest -R recording_recovery` 통과)
  - 정상 파일은 그대로이고 목록에 들어감
  - `.mp4` 꼬리가 잘림 → 해당 레코드부터 빠지고 두 파일이 잘림
  - 조각 바이트 하나가 바뀜 → 그 레코드부터 빠짐
  - `.idx` 마지막 레코드가 반쪽 → 그 레코드가 빠짐
  - 헤더 손상, 유효 레코드 없음, 짝 없는 `.mp4`·`.idx` → 지워짐
  - 이름 규칙에 맞지 않는 항목은 그대로

## 7. 명령행과 서버 연결

- [x] 7.1 `main.cpp`에 녹화 인자 다섯 개와 범위 검사, 카메라 이름 거부 규칙(`/` 포함, `.`, `..`, 255바이트 초과)을 추가하고 사용법 문구를 고친다
- [x] 7.2 `main.cpp`에 design 결정 10의 시작 순서(복구, 폴더 준비, `RecordingWriter`, 카메라마다 `CameraRecorder`)와 소멸 순서, 시작 로그(녹화 디렉터리와 설정값)를 구현한다. `.gitignore`에 `recordings/`를 추가한다
- [x] 7.3 `tests/integration/http_server_test.sh`에서 확인한다(`ctest -R integration` 통과). 카메라를 등록하는 기존 검사는 `--record-dir`로 임시 디렉터리를 쓰게 바꾼다
  - 시작 실패: `--record-segment 5`, `--record-retention`이 단위보다 작음, `--record-sync 0`, 숫자가 아닌 값
  - 시작 실패: 카메라 이름 `a/b`, `..`, 256바이트
  - 한글 카메라 이름으로 시작
  - 카메라 없이 실행하면 녹화 디렉터리가 생기지 않음
  - 카메라를 등록하면 `<녹화 디렉터리>/<이름>/`이 생김
  - 쓸 수 없는 녹화 디렉터리로 시작 실패(root로 실행하면 건너뜀)

## 8. 문서와 실제 카메라 검증

- [x] 8.1 README에 녹화 인자 표, 저장 구조(`recordings/<카메라>/<시각>.mp4`·`.idx`), 보관·비상 삭제 규칙, `ffprobe`·`ffplay` 확인 명령을 적는다. 적은 명령을 그대로 실행해 동작하는지 확인한다
  - 작업 메모: README의 `ffprobe -v error -show_entries stream=codec_name,width,height:format=duration -of default=nw=1 <파일>`을 실측 녹화 파일에 그대로 실행해 `h264`, `640`, `480`, `duration=60.000000`을 확인했다. `ffplay <파일>`은 화면 없이 `SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ffplay -autoexit -stats`로 60초 파일을 끝까지 재생했고 버린 프레임(fd)은 0이었다.
- [x] 8.2 vms-cam(`--test-source`)으로 확인한다
  - 3분 넘게 녹화하면 1분 남짓한 파일이 생기고, `ffprobe`가 H.264 스트림·해상도·길이를 보여 주며, `ffplay`로 재생됨
  - 10분 녹화 뒤 남은 닫힌 파일 길이 합이 300초 이하
  - vms-cam을 10초 끄고 켜면 앞 파일이 닫히고 새 파일이 생김
  - 녹화 중 브라우저로 카메라를 보는 동안 영상이 멈추지 않음
  - 작업 메모: vms-cam `--test-source`(640x480, 30fps, 키프레임 1초)로 약 11분 녹화했다. 3분 시점 파일 이름은 `125311Z`(시작), `125400Z`, `125500Z`, `125600Z`로 매분 0초였고, 닫힌 파일은 ffprobe 길이 60.000000초였다. vms-cam을 10초 끈 동안 `녹화 중단: cam (카메라 연결 끊김)` 뒤 `125600Z`가 닫히고 다시 연결된 뒤 `125656Z`가 새로 생겼다. 10분 시점 닫힌 파일은 60초 5개(합 300초)로 가장 오래된 파일부터 `보관 분량 초과로 삭제` 로그가 남았다. 모든 파일이 `ffmpeg -f null`로 오류 없이 디코딩되었다. 브라우저로 녹화 중 영상을 보는 항목은 Windows Chrome(WSL2 localhost 포워딩)으로 따로 확인했다. 카메라 cam을 WebRTC로 보는 동안 녹화 파일이 매분 0초에 바뀌어도(`141500Z`~`141700Z`, `141900Z`) 영상이 멈추지 않았다. 중간에 vms-cam을 끄자 `녹화 중단: cam (카메라 연결 끊김)` 뒤 `141800Z`가 47초 분량으로 닫혔고, 재연결 뒤 `녹화 시작: cam`과 함께 `141853Z`가 새로 생겼다. 브라우저 세션은 `카메라 대기 시작`과 `카메라 대기 끝` 로그 뒤 같은 연결로 다시 재생되었다. 이때 닫힌 파일 길이 합은 294초(60·60·60·47·7·60초)였고, `보관 분량 초과로 삭제` 로그와 함께 가장 오래된 파일부터 지워졌다.
- [x] 8.3 녹화 중 서버를 SIGKILL로 끝내고 다시 시작해, 복구 로그가 남고 남은 파일이 `ffprobe`로 열리며 새 파일에 이어서 녹화되는지 확인한다. SIGTERM으로 끝낸 뒤 다시 시작하면 잘라 낸 데이터가 없다는 로그가 나오는지도 확인한다
  - 작업 메모: 녹화 중 SIGKILL 뒤 다시 시작하자 `녹화 복구: cam, 남은 파일 6개, 잘라 낸 데이터 없음`이 남았고(쓰기 도중이 아니어서 잘라 낼 꼬리가 없었다), 남은 파일은 모두 ffprobe로 열렸으며 `130351Z` 새 파일에 이어서 녹화되었다. 잘린 꼬리를 자르는 경로는 `recording_recovery_test`가 확인한다. SIGTERM으로 끝낸 뒤(종료 코드 0) 다시 시작했을 때도 `잘라 낸 데이터 없음`이 나왔다.

## Workflow follow-up

- 검토 후 change를 아카이브하고, 아카이브 이동과 명세 반영 내용을 커밋하도록 제안한다.
