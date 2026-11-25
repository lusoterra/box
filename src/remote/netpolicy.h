#pragma once

// Default-deny gate for opportunistic network traffic.
//
// A fresh profile must make zero outbound connections before the user
// explicitly browses, downloads or opens Hydrus. Background conveniences
// (favicons, the Chromium capture profile, idle prefetch) check this gate
// and simply stay idle until it opens. The gate is session-only: it never
// persists, so every launch starts closed again.
#include <atomic>

namespace NetworkPolicy {
inline std::atomic<bool> &flag() {
    static std::atomic<bool> allowed{false};
    return allowed;
}

inline bool allowed() {
    return flag().load(std::memory_order_relaxed);
}

inline void allow() {
    flag().store(true, std::memory_order_relaxed);
}

// Tests only: put the process back into the fresh-launch state.
inline void reset() {
    flag().store(false, std::memory_order_relaxed);
}
} // namespace NetworkPolicy
