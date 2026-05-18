#pragma once

#include "app_types.h"

#include <optional>

namespace codexlimit
{

AppConfig LoadConfig();
std::optional<AuthTokens> LoadAuth(const AppConfig &cfg);
void SaveAuth(const AppConfig &cfg, const AuthTokens &tokens);
bool RefreshAuth(const AppConfig &cfg, AuthTokens &tokens);

} // namespace codexlimit
