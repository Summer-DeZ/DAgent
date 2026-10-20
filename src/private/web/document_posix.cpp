#include "web/detail.hpp"

#include <cerrno>
#include <iconv.h>
#include <stdexcept>

namespace dagent::web::detail {

std::string convert_charset(std::string body, const std::string& charset, bool truncated) {
    iconv_t converter = iconv_open("UTF-8", charset.c_str());
    if (converter == reinterpret_cast<iconv_t>(-1)) throw std::runtime_error("unsupported page charset: " + charset);
    std::string out(body.size() * 4 + 16, '\0');
    char* input = body.data(); char* output = out.data();
    size_t remaining = body.size(), available = out.size();
    while (remaining) {
        if (iconv(converter, &input, &remaining, &output, &available) != size_t(-1)) break;
        if (errno == EINVAL && truncated) break;
        if (errno != EILSEQ && errno != EINVAL) {
            iconv_close(converter); throw std::runtime_error("page charset conversion failed");
        }
        *output++ = '?'; --available; ++input; --remaining;
    }
    iconv_close(converter);
    out.resize(output - out.data());
    return out;
}

} // namespace dagent::web::detail
