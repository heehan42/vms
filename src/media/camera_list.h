#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "media/live_video_source.h"


namespace vms::media {


// 카메라 목록의 항목 하나.
struct CameraEntry {
    // 카메라 이름. --camera로 등록한 값이며 비어 있지 않은 UTF-8이다. 파일 동영상 이름과 겹칠 수 있다.
    std::string name;


    // 카메라 RTSP URL 원문. 로그에 쓴다.
    std::string url;


    // 이 카메라의 프레임을 세션들에 나눠 주는 실시간 소스
    LiveVideoSource source;
};


// 명령행으로 등록한 카메라 목록.
//
// 항목을 힙에 두고 소유하므로, Add·Find·Entries가 돌려준 항목과 그 소스의 주소는 이 객체가 살아 있는 동안 바뀌지
// 않는다. RTSP 클라이언트와 세션이 소스를 참조하므로 이 객체는 그들보다 오래 살아 있어야 한다. 시작할 때 항목을
// 모두 넣은 뒤에는 바꾸지 않는다.
class CameraList {
public:
    // 카메라를 목록 끝에 추가한다.
    //
    // name: 카메라 이름. 이미 있는 이름이 아니어야 한다. 중복 검사는 명령행 해석에서 한다.
    // url: 카메라 URL 원문
    // 반환값: 추가한 항목. 이 객체가 살아 있는 동안 유효하다.
    CameraEntry& Add(std::string name, std::string url);


    // 이름이 같은 카메라를 찾는다.
    //
    // name: 찾을 카메라 이름. 바이트 단위로 비교한다.
    // 반환값: 찾은 항목. 없으면 nullptr. 이 객체가 살아 있는 동안 유효하다.
    CameraEntry* Find(std::string_view name) const;


    // 등록 순서대로의 항목을 돌려준다.
    //
    // 반환값: 항목 목록. 이 객체가 살아 있는 동안 유효하다.
    const std::vector<std::unique_ptr<CameraEntry>>& Entries() const { return entries_; }


private:
    // 등록 순서대로의 항목. 소스 주소가 바뀌지 않도록 항목마다 힙에 둔다.
    std::vector<std::unique_ptr<CameraEntry>> entries_;
};


}  // namespace vms::media
