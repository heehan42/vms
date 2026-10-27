add_unit_test(stun_message_test)
add_unit_test(ice_lite_agent_test)
add_unit_test(sdp_test)
# 실제 Chrome offer 견본을 읽을 수 있도록 데이터 디렉터리 경로를 넘긴다.
target_compile_definitions(sdp_test PRIVATE SDP_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
