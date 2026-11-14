#pragma once

#include <mutex>
#include <pthread.h>

// std::mutex::lock() formats a std::system_error and throws when
// pthread_mutex_lock fails. Exceptions are disabled, so that throw aborts in
// __cxa_allocate_exception. Return the error instead.
inline bool LockIfAvailable(pthread_mutex_t* mutex) noexcept {
    return mutex != nullptr && pthread_mutex_lock(mutex) == 0;
}

inline void UnlockIfHeld(pthread_mutex_t* mutex, bool held) noexcept {
    if (held && mutex != nullptr) {
        pthread_mutex_unlock(mutex);
    }
}

class NothrowLock {
public:
    explicit NothrowLock(std::mutex& mutex) noexcept : mutex_(mutex) {
        held_ = LockIfAvailable(mutex_.native_handle());
    }

    ~NothrowLock() { UnlockIfHeld(mutex_.native_handle(), held_); }

    explicit operator bool() const noexcept { return held_; }

    NothrowLock(const NothrowLock&) = delete;
    NothrowLock& operator=(const NothrowLock&) = delete;

private:
    std::mutex& mutex_;
    bool held_ = false;
};
