#pragma once
#include <string>
#include <string_view>
namespace qtchat {
struct PasswordHash {
  std::string saltHex, hashHex;
  int iterations = 600000;
};
PasswordHash hashPassword(std::string_view password);
bool verifyPassword(std::string_view password, const PasswordHash &hash);
} // namespace qtchat
