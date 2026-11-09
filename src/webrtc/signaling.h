#pragma once

#include <string_view>

#include "crypto/certificate.h"
#include "http/http_request.h"
#include "http/http_server.h"
#include "media/camera_list.h"
#include "media/video_library.h"
#include "webrtc/ice_lite_agent.h"


namespace vms::webrtc {


// 웹 페이지가 WebRTC 연결을 시작할 때 쓰는 POST /offer 엔드포인트.
//
// 브라우저의 SDP offer와 고른 소스의 종류·이름을 해석해 ICE 세션을 만들고 SDP answer로 응답한다. 소스 목록
// (GET /api/sources)은 녹화 소스까지 담으므로 playback::BuildSourceListResponse가 만든다.
// ICE-lite 에이전트를 같은 프로세스에서 직접 호출한다. 에이전트, 인증서, 동영상·카메라 목록은 참조만 하며
// 소유하지 않으므로, 모두 이 객체보다 오래 살아 있어야 한다. 이 객체를 핸들러로 등록한 HttpServer는
// 이 객체보다 먼저 소멸해야 한다.
class Signaling {
public:
    // 세션을 만들 에이전트, fingerprint를 알릴 인증서, 고를 수 있는 동영상·카메라 목록으로 엔드포인트를 만든다.
    //
    // agent: 세션을 만들고 candidate를 제공하는 ICE-lite 에이전트
    // certificate: answer의 a=fingerprint에 넣을 서버 인증서
    // video_library: offer의 type=file 소스를 name으로 찾을 동영상 목록. 동영상 없이 실행하면 빈 목록이다.
    //   세션이 항목의 파일을 참조하므로 에이전트보다 오래 살아 있어야 한다.
    // camera_list: offer의 type=camera 소스를 name으로 찾을 카메라 목록. 카메라 없이 실행하면 빈 목록이다.
    //   세션이 항목의 소스를 구독하므로 에이전트보다 오래 살아 있어야 한다.
    Signaling(IceLiteAgent& agent, const crypto::Certificate& certificate, const media::VideoLibrary& video_library,
              const media::CameraList& camera_list);


    // offer 요청 하나를 처리한다. HttpServer의 POST 핸들러로 등록해 쓴다.
    //
    // request: 파싱한 요청 헤더. 요청 대상의 type(소스 종류)·name(소스 이름) 쿼리 매개변수로 세션에 보낼 소스를
    //   정한다. 다른 이름의 매개변수(이전 형식의 video 포함)는 무시한다.
    // body: offer SDP 본문
    // 반환값: 처리 결과
    //   - 200, application/sdp, answer: 세션을 만들고 answer를 생성함. type·name이 모두 없으면 소스 없는 세션
    //   - 415: Content-Type이 application/sdp가 아님
    //   - 400: type·name의 퍼센트 인코딩이 잘못되었거나 두 번 이상 옴, 둘 중 하나만 옴, type이 file·camera가
    //     아님(원인을 로그에 남김), 또는 SDP 형식이 잘못되었거나
    //     협상할 비디오·H.264·ICE 자격 증명·sha-256 fingerprint가 없거나, a=setup이 actpass·active가
    //     아니거나, H.264 payload type이 0~127 정수가 아님(원인을 로그에 남김)
    //   - 404: 종류와 이름이 모두 같은 소스가 목록에 없음(원인을 로그에 남김). 카메라는 연결 상태와 관계없이
    //     목록에 있으면 찾는다.
    //   - 500: 난수 생성 실패(원인을 로그에 남김)
    //
    // 판정 순서는 Content-Type, 소스(해석 오류 → 한쪽만 옴 → 종류 → 목록 존재), SDP다. 오류 응답이면 세션을 만들지 않는다.
    http::HttpEndpointResponse HandleOffer(const http::HttpRequest& request, std::string_view body);


private:
    // 세션을 만들 ICE-lite 에이전트
    IceLiteAgent& agent_;


    // fingerprint를 알릴 서버 인증서
    const crypto::Certificate& certificate_;


    // offer의 type=file 소스를 name으로 찾을 동영상 목록
    const media::VideoLibrary& video_library_;


    // offer의 type=camera 소스를 name으로 찾을 카메라 목록
    const media::CameraList& camera_list_;
};


}  // namespace vms::webrtc
