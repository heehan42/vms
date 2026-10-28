#pragma once

#include <string_view>

#include "crypto/certificate.h"
#include "http/http_request.h"
#include "http/http_server.h"
#include "webrtc/ice_lite_agent.h"


namespace vms::webrtc {


// POST /offer 엔드포인트. 브라우저의 SDP offer를 해석해 ICE 세션을 만들고 SDP answer로 응답한다.
//
// ICE-lite 에이전트를 같은 프로세스에서 직접 호출한다. 에이전트와 인증서는 참조만 하며 소유하지 않으므로,
// 둘 다 이 객체보다 오래 살아 있어야 한다. 이 객체를 핸들러로 등록한 HttpServer는 이 객체보다 먼저
// 소멸해야 한다.
class Signaling {
public:
    // 세션을 만들 에이전트와 fingerprint를 알릴 인증서로 엔드포인트를 만든다.
    //
    // agent: 세션을 만들고 candidate를 제공하는 ICE-lite 에이전트
    // certificate: answer의 a=fingerprint에 넣을 서버 인증서
    Signaling(IceLiteAgent& agent, const crypto::Certificate& certificate);


    // offer 요청 하나를 처리한다. HttpServer의 POST 핸들러로 등록해 쓴다.
    //
    // request: 파싱한 요청 헤더
    // body: offer SDP 본문
    // 반환값: 처리 결과
    //   - 200, application/sdp, answer: 세션을 만들고 answer를 생성함
    //   - 415: Content-Type이 application/sdp가 아님
    //   - 400: SDP 형식이 잘못되었거나 협상할 비디오·H.264·ICE 자격 증명·sha-256 fingerprint가 없거나,
    //     a=setup이 actpass·active가 아님(원인을 로그에 남김)
    //   - 500: 난수 생성 실패(원인을 로그에 남김)
    http::HttpEndpointResponse HandleOffer(const http::HttpRequest& request, std::string_view body);


private:
    // 세션을 만들 ICE-lite 에이전트
    IceLiteAgent& agent_;


    // fingerprint를 알릴 서버 인증서
    const crypto::Certificate& certificate_;
};


}  // namespace vms::webrtc
