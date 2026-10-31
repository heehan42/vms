add_unit_test(random_test)
add_unit_test(hmac_sha1_test)
add_unit_test(certificate_test)
add_unit_test(dtls_context_test)

# 브라우저 역할로 SRTP 패킷을 검증·복호화하는 테스트 전용 수신 측. 서버 코드(server_core)에는 넣지 않는다.
add_library(srtp_test_receiver STATIC srtp_test_receiver.cpp)
target_link_libraries(srtp_test_receiver PUBLIC server_core)
target_include_directories(srtp_test_receiver PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
add_unit_test(srtp_test)
target_link_libraries(srtp_test PRIVATE srtp_test_receiver)
