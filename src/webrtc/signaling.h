#pragma once

#include <string_view>

#include "crypto/certificate.h"
#include "http/http_request.h"
#include "http/http_server.h"
#include "media/video_library.h"
#include "webrtc/ice_lite_agent.h"


namespace vms::webrtc {


// 웹 페이지가 쓰는 API 엔드포인트. POST /offer와 GET /api/videos를 처리한다.
//
// POST /offer는 브라우저의 SDP offer와 고른 동영상 ID를 해석해 ICE 세션을 만들고 SDP answer로 응답한다.
// GET /api/videos는 동영상 목록을 JSON으로 응답한다.
// ICE-lite 에이전트를 같은 프로세스에서 직접 호출한다. 에이전트, 인증서, 동영상 목록은 참조만 하며
// 소유하지 않으므로, 모두 이 객체보다 오래 살아 있어야 한다. 이 객체를 핸들러로 등록한 HttpServer는
// 이 객체보다 먼저 소멸해야 한다.
class Signaling {
public:
    // 세션을 만들 에이전트, fingerprint를 알릴 인증서, 고를 수 있는 동영상 목록으로 엔드포인트를 만든다.
    //
    // agent: 세션을 만들고 candidate를 제공하는 ICE-lite 에이전트
    // certificate: answer의 a=fingerprint에 넣을 서버 인증서
    // video_library: offer의 video 매개변수로 찾을 동영상 목록. 동영상 없이 실행하면 빈 목록이다.
    //   세션이 항목의 파일을 참조하므로 에이전트보다 오래 살아 있어야 한다.
    Signaling(IceLiteAgent& agent, const crypto::Certificate& certificate, const media::VideoLibrary& video_library);


    // offer 요청 하나를 처리한다. HttpServer의 POST 핸들러로 등록해 쓴다.
    //
    // request: 파싱한 요청 헤더. 요청 대상의 video 쿼리 매개변수로 세션에 보낼 동영상을 정한다.
    // body: offer SDP 본문
    // 반환값: 처리 결과
    //   - 200, application/sdp, answer: 세션을 만들고 answer를 생성함. video가 없으면 동영상 없는 세션
    //   - 415: Content-Type이 application/sdp가 아님
    //   - 400: video의 퍼센트 인코딩이 잘못되었거나 video가 두 번 이상 옴, 또는 SDP 형식이 잘못되었거나
    //     협상할 비디오·H.264·ICE 자격 증명·sha-256 fingerprint가 없거나, a=setup이 actpass·active가
    //     아니거나, H.264 payload type이 0~127 정수가 아님(원인을 로그에 남김)
    //   - 404: video 값이 목록의 어떤 ID와도 같지 않음(원인을 로그에 남김)
    //   - 500: 난수 생성 실패(원인을 로그에 남김)
    //
    // 판정 순서는 Content-Type, video, SDP다. 오류 응답이면 세션을 만들지 않는다.
    http::HttpEndpointResponse HandleOffer(const http::HttpRequest& request, std::string_view body);


    // 동영상 목록 요청 하나를 처리한다. HttpServer의 GET 핸들러로 등록해 쓴다.
    //
    // 반환값: 200, application/json, {"videos":[{"id":"...","frames":N},...]}. 원소는 목록 순서이며,
    //   동영상이 없으면 빈 배열이다.
    http::HttpEndpointResponse HandleVideoList() const;


private:
    // 세션을 만들 ICE-lite 에이전트
    IceLiteAgent& agent_;


    // fingerprint를 알릴 서버 인증서
    const crypto::Certificate& certificate_;


    // offer의 video 매개변수로 찾을 동영상 목록
    const media::VideoLibrary& video_library_;
};


}  // namespace vms::webrtc
