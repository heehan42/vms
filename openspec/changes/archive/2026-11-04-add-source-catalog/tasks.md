# Tasks

## 1. 시그널링의 소스 선택과 목록

- [x] 1.1 `src/webrtc/signaling.cpp`에 `SourceType`과 `type` 문자열 해석을 추가하고, `FindRequestedVideo`를 design 결정 2의 순서로 판정하는 `FindRequestedSource`로 바꾼다. 원인별 거부 로그를 남기고, `signaling.h`의 `HandleOffer` 주석(매개변수, 반환 코드, 판정 순서)을 고친다. 빌드가 통과하는지 확인한다
- [x] 1.2 `HandleVideoList`를 `HandleSourceList`로 바꿔 `{"sources":[{"type":"file","id":...,"frames":...}]}`를 만들고, 헤더 주석과 클래스 주석의 엔드포인트 경로를 고친다. 빌드가 통과하는지 확인한다
- [x] 1.3 `tests/webrtc/signaling_test.cpp`를 새 형식으로 고친다. 다음을 확인한다(`ctest -R signaling` 통과)
  - 200: `type=file&id=a.h264`, 인코딩된 한글 ID, 쿼리 없음, `video=a.h264`만 있음, `video` 중복
  - 404: `type=file&id=missing.h264`, `..%2F` ID, 대소문자가 다른 ID, `type=camera&id=a.h264`
  - 400: `type=disk`, `id`만 있음, `type`만 있음, `id=%zz`, `id` 중복, `type` 중복
  - 판정 순서: Content-Type → 소스 → SDP
  - 목록 JSON의 순서, `type`, `frames`, 이스케이프, 빈 목록 `{"sources":[]}`

## 2. 엔드포인트 등록과 통합 테스트

- [x] 2.1 `src/apps/web_server/main.cpp`에서 GET 엔드포인트를 `/api/sources` → `HandleSourceList`로 바꾸고 빌드가 통과하는지 확인한다
- [x] 2.2 `tests/integration/http_server_test.sh`를 고친다. 다음을 확인한다(`ctest -R integration` 통과)
  - `--video-dir` 없이 `GET /api/sources`는 `{"sources":[]}`이고 `Content-Type`은 `application/json`
  - `POST /api/sources`는 `Allow: GET`과 함께 405
  - `GET /api/videos`는 404
  - `POST /offer?type=file&id=missing.h264`는 404, `?type=file&id=%zz`는 400, `?id=a.h264`는 400
  - 디렉터리로 실행했을 때 목록 JSON과 `?type=file&id=a.h264`는 200

## 3. 웹 페이지와 문서

- [x] 3.1 `web/index.html`의 `select` 요소를 `source-select`와 "소스" 레이블로 바꾸고 주석을 `/api/sources` 기준으로 고친다. `web/app.js`는 `/api/sources`로 목록을 받아 배열을 보관하고, `option`에 "파일 · ID"처럼 종류 이름과 ID를 표시하고 값에는 인덱스를 넣는다. 연결할 때 `type`과 `id`를 붙이고, 빈 목록이면 "소스 없음"을 표시한다(design 결정 4)
- [x] 3.2 README의 확인 절차와 API 설명을 `/api/sources`와 `type`·`id` 형식에 맞게 고친다. 적은 `curl` 명령을 실행해 문서와 같은 응답이 나오는지 확인한다

## 4. 통합 검증

- [x] 4.1 Windows Chrome에서 확인한다. 다음이 모두 맞아야 한다
  - `select` 요소에 "파일 · ..." 항목 두 개가 이름순으로 나온다
  - 각 항목을 골라 연결하면 그 영상이 재생된다
  - 재생 중 다른 항목으로 다시 연결하면 새 영상이 처음부터 재생된다
  - `--video-dir` 없이 실행하면 "소스 없음"이 표시되고, 연결 상태는 `connected`가 되며 영상은 나오지 않는다

## Workflow follow-up

- 검토 후 change를 아카이브하고, 아카이브 이동과 명세 반영 내용을 커밋하도록 제안한다.
- 이어서 `add-rtsp-camera-source` change를 제안한다.
