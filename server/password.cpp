#include "password.h"
#include <array>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <stdexcept>
#include <vector>
namespace qtchat {
namespace {
std::string hex(const unsigned char *bytes, std::size_t size) {
  constexpr char digits[] = "0123456789abcdef";
  std::string text;
  text.reserve(size * 2);
  for (std::size_t i = 0; i < size; ++i) {
    text += digits[bytes[i] >> 4];
    text += digits[bytes[i] & 15];
  }
  return text;
}
std::vector<unsigned char> unhex(std::string_view text) {
  if (text.size() % 2)
    return {};
  auto digit = [](char c) -> int {
    if (c >= '0' && c <= '9')
      return c - '0';
    if (c >= 'a' && c <= 'f')
      return c - 'a' + 10;
    return -1;
  };
  std::vector<unsigned char> bytes;
  for (std::size_t i = 0; i < text.size(); i += 2) {
    int a = digit(text[i]), b = digit(text[i + 1]);
    if (a < 0 || b < 0)
      return {};
    bytes.push_back(static_cast<unsigned char>(a * 16 + b));
  }
  return bytes;
}
std::array<unsigned char, 32>
derive(std::string_view password, const unsigned char *salt, int iterations) {
  std::array<unsigned char, 32> bytes{};
  if (PKCS5_PBKDF2_HMAC(password.data(), static_cast<int>(password.size()),
                        salt, 16, iterations, EVP_sha256(), 32,
                        bytes.data()) != 1)
    throw std::runtime_error("password derivation failed");
  return bytes;
}
} // namespace
PasswordHash hashPassword(std::string_view password) {
  if (password.size() < 8 || password.size() > 128)
    throw std::invalid_argument("password must be 8..128 bytes");
  std::array<unsigned char, 16> salt{};
  if (RAND_bytes(salt.data(), static_cast<int>(salt.size())) != 1)
    throw std::runtime_error("secure random generation failed");
  auto bytes = derive(password, salt.data(), 600000);
  return {hex(salt.data(), salt.size()), hex(bytes.data(), bytes.size()),
          600000};
}
bool verifyPassword(std::string_view password, const PasswordHash &hash) {
  if (password.size() < 8 || password.size() > 128 ||
      hash.iterations < 100000 || hash.iterations > 5000000)
    return false;
  const auto salt = unhex(hash.saltHex), expected = unhex(hash.hashHex);
  if (salt.size() != 16 || expected.size() != 32)
    return false;
  const auto actual = derive(password, salt.data(), hash.iterations);
  return CRYPTO_memcmp(actual.data(), expected.data(), actual.size()) == 0;
}
} // namespace qtchat
