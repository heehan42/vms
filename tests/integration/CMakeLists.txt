# 합성 프레임으로 닫힌 녹화 파일 짝을 만드는 도구. 통합 시험이 녹화 재생 시나리오의 녹화 디렉터리를 만드는 데 쓴다.
add_executable(make_test_recording ${CMAKE_CURRENT_SOURCE_DIR}/make_test_recording.cpp)
target_link_libraries(make_test_recording PRIVATE server_core)
target_include_directories(make_test_recording PRIVATE ${PROJECT_SOURCE_DIR}/tests)

# 실제 서버를 띄워 http-server, webrtc-signaling, video-catalog, recording-playback 명세의 시나리오를 요청으로 검사한다.
add_test(NAME http_server_test
         COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/http_server_test.sh $<TARGET_FILE:vms_web_server>
                 ${PROJECT_SOURCE_DIR}/tests/webrtc/data/chrome_offer.sdp $<TARGET_FILE:make_test_recording>)
