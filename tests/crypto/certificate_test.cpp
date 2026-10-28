#include "crypto/certificate.h"

#include <string>

#include "test_support.h"

using vms::crypto::Certificate;
using vms::crypto::ComputeSha256Fingerprint;
using vms::test::Expect;


namespace {


// fingerprint가 "AB:CD:..." 형식(대문자 16진수 32쌍, ':' 구분)인지 확인한다.
//
// fingerprint: 검사할 문자열
// 반환값: 95자이고 3번째 문자마다 ':', 나머지는 대문자 16진수이면 true
bool IsSha256FingerprintFormat(const std::string& fingerprint) {
    if (fingerprint.size() != 32 * 3 - 1) {
        return false;
    }
    for (std::size_t index = 0; index < fingerprint.size(); ++index) {
        const char character = fingerprint[index];
        const bool is_separator_position = index % 3 == 2;
        const bool is_upper_hex =
            (character >= '0' && character <= '9') || (character >= 'A' && character <= 'F');
        if (is_separator_position ? character != ':' : !is_upper_hex) {
            return false;
        }
    }
    return true;
}


// 인증서를 만들고 fingerprint가 SDP 형식인지, 조회할 때마다 같은지 검사한다.
void TestGeneratesFingerprint() {
    const auto certificate = Certificate::Generate();
    if (!certificate) {
        Expect(false, "인증서 생성이 성공해야 한다: " + certificate.error());
        return;
    }
    Expect(IsSha256FingerprintFormat(certificate->Sha256Fingerprint()),
           "fingerprint 형식: " + certificate->Sha256Fingerprint());
    Expect(certificate->Sha256Fingerprint() == certificate->Sha256Fingerprint(),
           "같은 인증서의 fingerprint는 같아야 한다");
}


// 두 번 만든 인증서는 키와 일련번호가 달라 fingerprint도 다른지 검사한다.
void TestGeneratesDifferentCertificates() {
    const auto first = Certificate::Generate();
    const auto second = Certificate::Generate();
    Expect(first && second && first->Sha256Fingerprint() != second->Sha256Fingerprint(),
           "새로 만든 인증서의 fingerprint는 달라야 한다");
}


// 접근자로 꺼낸 인증서의 fingerprint를 다시 계산하면 보관한 값과 같은지, 키가 인증서와 짝인지 검사한다.
void TestAccessorsMatchStoredFingerprint() {
    const auto certificate = Certificate::Generate();
    if (!certificate) {
        Expect(false, "인증서 생성이 성공해야 한다: " + certificate.error());
        return;
    }
    const auto recomputed = ComputeSha256Fingerprint(certificate->X509Certificate());
    Expect(recomputed && *recomputed == certificate->Sha256Fingerprint(),
           "같은 인증서는 두 경로의 fingerprint가 같아야 한다");
    Expect(::X509_check_private_key(certificate->X509Certificate(), certificate->PrivateKey()) == 1,
           "개인 키가 인증서의 공개 키와 짝이어야 한다");
}


}  // namespace


int main() {
    TestGeneratesFingerprint();
    TestGeneratesDifferentCertificates();
    TestAccessorsMatchStoredFingerprint();
    return vms::test::ExitCode();
}
