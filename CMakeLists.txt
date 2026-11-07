cmake_minimum_required(VERSION 3.28)

project(vms LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_compile_options(-fno-exceptions -Wall -Wextra -Wpedantic)

# 시스템에 설치된 OpenSSL(Ubuntu의 libssl-dev)을 사용한다. 예외를 던지지 않는 C API만 쓴다.
# DTLS 데이터그램 처리에 3.2부터 들어간 datagram pair BIO를 쓰므로 3.2 이상이 필요하다.
find_package(OpenSSL 3.2 REQUIRED)

# 녹화 스레드(std::thread)에 필요한 스레드 라이브러리
find_package(Threads REQUIRED)

# 실행 파일과 테스트가 함께 링크하는 서버 구성 요소
add_library(server_core STATIC
    src/core/event_loop.cpp
    src/core/json.cpp
    src/core/network_interfaces.cpp
    src/core/termination_signal_watcher.cpp
    src/crypto/certificate.cpp
    src/crypto/dtls_context.cpp
    src/crypto/hmac_sha1.cpp
    src/crypto/openssl_error.cpp
    src/crypto/random.cpp
    src/crypto/srtp.cpp
    src/http/http_request.cpp
    src/http/http_response.cpp
    src/http/http_server.cpp
    src/http/mime_type.cpp
    src/http/query_string.cpp
    src/http/static_files.cpp
    src/media/camera_list.cpp
    src/media/h264_file.cpp
    src/media/h264_sps.cpp
    src/media/live_video_source.cpp
    src/media/video_library.cpp
    src/recording/recording_file_name.cpp
    src/recording/camera_recorder.cpp
    src/recording/camera_recording_planner.cpp
    src/recording/fmp4.cpp
    src/recording/recording_index.cpp
    src/recording/recording_file_store.cpp
    src/recording/recording_recovery.cpp
    src/recording/recording_writer.cpp
    src/rtsp/h264_rtp_depacketizer.cpp
    src/rtsp/rtsp_camera_client.cpp
    src/rtsp/rtsp_message.cpp
    src/rtsp/rtsp_url.cpp
    src/webrtc/dtls_transport.cpp
    src/webrtc/h264_packetizer.cpp
    src/webrtc/ice_lite_agent.cpp
    src/webrtc/live_video_stream_sender.cpp
    src/webrtc/rtcp_packet.cpp
    src/webrtc/rtp_packet.cpp
    src/webrtc/sdp.cpp
    src/webrtc/signaling.cpp
    src/webrtc/srtp_video_packet_writer.cpp
    src/webrtc/stun_message.cpp
    src/webrtc/video_stream_sender.cpp
)
target_include_directories(server_core PUBLIC src)
target_link_libraries(server_core PUBLIC OpenSSL::SSL OpenSSL::Crypto Threads::Threads)

add_executable(vms_web_server src/apps/web_server/main.cpp)
target_link_libraries(vms_web_server PRIVATE server_core)

enable_testing()
add_subdirectory(tests)
