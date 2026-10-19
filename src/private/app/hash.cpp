#include "app/hash.hpp"

#include <array>
#include <iomanip>
#include <sstream>
#include <stdexcept>

#include <openssl/evp.h>

namespace dagent::app {

std::string sha256(std::string_view data) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> bytes{};
    unsigned int length = 0;
    if (EVP_Digest(data.data(), data.size(), bytes.data(), &length, EVP_sha256(), nullptr) != 1)
        throw std::runtime_error("SHA-256 failed");
    std::ostringstream out;
    for (unsigned int i = 0; i < length; ++i) out << std::hex << std::setw(2) << std::setfill('0') << unsigned(bytes[i]);
    return out.str();
}

} // namespace dagent::app
