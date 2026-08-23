#include "catch.hpp"
#include <string>
#include "romm/md5.hpp"

// MD5 test vectors from RFC 1321, section A.5 (verbatim).

TEST_CASE("md5Hex RFC vector empty string") {
    std::string hex;
    REQUIRE(romm::md5Hex("", hex));
    REQUIRE(hex == "d41d8cd98f00b204e9800998ecf8427e");
}

TEST_CASE("md5Hex RFC vector a") {
    std::string hex;
    REQUIRE(romm::md5Hex("a", hex));
    REQUIRE(hex == "0cc175b9c0f1b6a831c399e269772661");
}

TEST_CASE("md5Hex RFC vector abc") {
    std::string hex;
    REQUIRE(romm::md5Hex("abc", hex));
    REQUIRE(hex == "900150983cd24fb0d6963f7d28e17f72");
}

TEST_CASE("md5Hex RFC vector message digest") {
    std::string hex;
    REQUIRE(romm::md5Hex("message digest", hex));
    REQUIRE(hex == "f96b697d7cb7938d525a2f31aaf161d0");
}

TEST_CASE("md5Hex RFC vector lowercase alphabet") {
    std::string hex;
    REQUIRE(romm::md5Hex("abcdefghijklmnopqrstuvwxyz", hex));
    REQUIRE(hex == "c3fcd3d76192e4007dfb496cca67e13b");
}

TEST_CASE("md5Hex RFC vector mixed case alphanumeric") {
    std::string hex;
    const char* s = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    REQUIRE(romm::md5Hex(s, hex));
    REQUIRE(hex == "d174ab98d277d9f5a5611c2c9f419d9f");
}

TEST_CASE("md5Hex RFC vector digits") {
    std::string hex;
    const char* s = "12345678901234567890123456789012345678901234567890123456789012345678901234567890";
    REQUIRE(romm::md5Hex(s, hex));
    REQUIRE(hex == "57edf4a22be3c955ac49da2e2107b67a");
}

