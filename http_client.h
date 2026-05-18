#pragma once

#include "app_types.h"

#include <optional>
#include <string>

namespace codexlimit
{

std::optional<HttpResponse> HttpRequest(const std::wstring &method,
                                        const std::wstring &url,
                                        const std::wstring &headers,
                                        const std::string &body = {});

std::optional<HttpResponse> HttpRequestOfficialFirst(const std::wstring &method,
                                                     const std::wstring &url,
                                                     const std::wstring &headers,
                                                     const std::string &body = {});

} // namespace codexlimit
