#include "crypto/openssl_error.h"

#include <openssl/err.h>

#include <array>
#include <format>


namespace vms::crypto {


std::string TakeOpenSslError(std::string_view operation) {
    std::string message = std::format("{}: ", operation);

    bool is_first = true;
    while (const unsigned long error = ::ERR_get_error()) {
        std::array<char, 256> text{};
        ::ERR_error_string_n(error, text.data(), text.size());
        if (!is_first) {
            message += "; ";
        }
        message += text.data();
        is_first = false;
    }
    if (is_first) {
        message += "원인 불명";
    }
    return message;
}


}  // namespace vms::crypto
