#pragma once

#ifdef __x86_64__
#include <immintrin.h>
#else
#include <thread>
#endif

namespace exchange {

// To be called in a loop that waits for another thread without sleeping. On
// x86 the PAUSE instruction tells the processor that this is a waiting loop:
// it saves power and leaves more resources to the other hardware thread of
// the same core.
inline void cpu_relax() noexcept {
#ifdef __x86_64__
    _mm_pause();
#else
    std::this_thread::yield();
#endif
}

} // namespace exchange
