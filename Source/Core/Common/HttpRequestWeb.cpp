// Copyright 2026 dolphin2web Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The web build has no libcurl. Dolphin only uses HTTP for optional online features
// (update checks, analytics, achievements), so every request simply fails.

#include "Common/HttpRequest.h"

#include <cctype>

#include <fmt/format.h>

namespace Common
{
class HttpRequest::Impl
{
};

HttpRequest::HttpRequest(std::chrono::milliseconds, ProgressCallback)
{
}

HttpRequest::~HttpRequest() = default;

bool HttpRequest::IsValid() const
{
  return false;
}

void HttpRequest::SetCookies(const std::string&)
{
}

void HttpRequest::UseIPv4()
{
}

void HttpRequest::FollowRedirects(long)
{
}

std::string HttpRequest::EscapeComponent(const std::string& string)
{
  std::string out;
  for (const unsigned char c : string)
  {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
      out += static_cast<char>(c);
    else
      out += fmt::format("%{:02X}", c);
  }
  return out;
}

s32 HttpRequest::GetLastResponseCode() const
{
  return 0;
}

std::string HttpRequest::GetHeaderValue(std::string_view) const
{
  return {};
}

HttpRequest::Response HttpRequest::Get(const std::string&, const Headers&, AllowedReturnCodes)
{
  return {};
}

HttpRequest::Response HttpRequest::Post(const std::string&, std::span<const u8>, const Headers&,
                                        AllowedReturnCodes)
{
  return {};
}

HttpRequest::Response HttpRequest::Post(const std::string&, std::string_view, const Headers&,
                                        AllowedReturnCodes)
{
  return {};
}

HttpRequest::Response HttpRequest::PostMultiform(const std::string&, std::span<Multiform>,
                                                 const Headers&, AllowedReturnCodes)
{
  return {};
}
}  // namespace Common
