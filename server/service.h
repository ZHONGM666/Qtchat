#pragma once
#include "database.h"
#include <cstdint>
#include <optional>
#include <vector>
namespace qtchat {
struct RequestContext {
  std::uint64_t connectionId = 0;
  std::int64_t userId = 0;
};
struct Push {
  std::int64_t userId;
  nlohmann::json event;
};
struct ServiceResult {
  nlohmann::json reply;
  std::optional<std::int64_t> authenticatedUser;
  bool logout = false;
  std::vector<Push> pushes;
};
class Service {
public:
  explicit Service(Database &database) : database_(database) {}
  ServiceResult execute(RequestContext context, const nlohmann::json &request);
  std::vector<std::int64_t> friendIds(std::int64_t userId);

private:
  Database &database_;
};
} // namespace qtchat
