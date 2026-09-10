#pragma once
#include <nlohmann/json.hpp>
#include <charconv>
#include <string>
namespace fan {
inline nlohmann::json draftNumber(const std::string& text) {
    int n=0;auto [end,ec]=std::from_chars(text.data(),text.data()+text.size(),n);
    return ec==std::errc{} && end==text.data()+text.size()?nlohmann::json(n):nlohmann::json(text);
}
inline std::string draftText(const nlohmann::json& value) {
    return value.is_string()?value.get<std::string>():value.dump();
}
}
