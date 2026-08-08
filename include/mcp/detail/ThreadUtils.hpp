#pragma once
// ThreadUtils.hpp - thread join helpers
#include <thread>

namespace mcp { namespace detail {
inline void JoinThreadSafely(std::thread& t) {
    if (!t.joinable()) return;
    if (t.get_id() == std::this_thread::get_id()) t.detach();
    else t.join();
}
}}
