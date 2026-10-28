add_unit_test(stun_message_test)
add_unit_test(ice_lite_agent_test)
add_unit_test(sdp_test)
# 실제 Chrome offer 견본을 읽을 수 있도록 데이터 디렉터리 경로를 넘긴다.
target_compile_definitions(sdp_test PRIVATE SDP_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")

# 브라우저 역할을 하는 테스트 전용 DTLS 클라이언트. 서버 코드(server_core)에는 넣지 않는다.
add_library(dtls_test_client STATIC dtls_test_client.cpp)
target_link_libraries(dtls_test_client PUBLIC server_core)
target_include_directories(dtls_test_client PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
add_unit_test(dtls_transport_test)
target_link_libraries(dtls_transport_test PRIVATE dtls_test_client)
target_link_libraries(ice_lite_agent_test PRIVATE dtls_test_client)
