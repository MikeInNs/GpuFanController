#pragma once
#include "fan/ApiClient.hpp"

namespace fan {
class TerminalUi {
public:
    explicit TerminalUi(const ApiClient& client) : client_(client) {}
    int run();
private:
    const ApiClient& client_;
};
}
