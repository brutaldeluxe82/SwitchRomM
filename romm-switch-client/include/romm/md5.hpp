#pragma once

#include <cstddef>
#include <cstring>
#include <string>

namespace romm {

// Compute the MD5 digest of `data` as lowercase hex. Returns false only on
// unreasonable output (should not happen). No exceptions; the only allocation
// is the caller-provided outHex string.
bool md5Hex(const unsigned char* data, size_t len, std::string& outHex);

bool md5Hex(const std::string& data, std::string& outHex);

} // namespace romm
