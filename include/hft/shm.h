// SPDX-License-Identifier: MIT
// shm.h - Named shared memory wrapper (Windows first, POSIX fallback).
#ifndef HFT_SHM_H
#define HFT_SHM_H

#include "hft/platform.h"

#include <new>
#include <string>
#include <utility>

#if defined(HFT_POSIX)
#  include <cerrno>
#  include <sys/stat.h>
#endif

namespace hft {

// A raw named shared-memory mapping.
//
// On Windows the object is backed by the kernel (CreateFileMapping) and lives
// until the last handle is closed, so no explicit clean-up is required when all
// processes exit. On POSIX we keep the segment in /dev/shm and unlink it from
// the creator process.
class SharedMemory {
public:
    SharedMemory() = default;
    ~SharedMemory() { close(); }

    SharedMemory(const SharedMemory&) = delete;
    SharedMemory& operator=(const SharedMemory&) = delete;

    SharedMemory(SharedMemory&& other) noexcept { swap(other); }
    SharedMemory& operator=(SharedMemory&& other) noexcept {
        if (this != &other) {
            close();
            swap(other);
        }
        return *this;
    }

    // Creates the region (or attaches to it when it already exists).
    // Returns false on failure; call lastError() for a human readable reason.
    bool create(const std::string& name, uint64_t size);

    // Attaches to an existing region; never creates one.
    bool open(const std::string& name, uint64_t size);

    bool isOpen() const { return addr_ != nullptr; }
    bool isCreator() const { return creator_; }
    void* address() const { return addr_; }
    uint64_t size() const { return size_; }
    const std::string& name() const { return name_; }
    const std::string& lastError() const { return lastError_; }

    template <typename T>
    T* as() const { return reinterpret_cast<T*>(addr_); }

    void close();

    void swap(SharedMemory& other) noexcept {
        std::swap(addr_, other.addr_);
        std::swap(size_, other.size_);
        std::swap(creator_, other.creator_);
        std::swap(name_, other.name_);
        std::swap(lastError_, other.lastError_);
#if defined(HFT_WINDOWS)
        std::swap(handle_, other.handle_);
#else
        std::swap(fd_, other.fd_);
        std::swap(shmName_, other.shmName_);
#endif
    }

private:
    void setError(const char* what);

    void* addr_ = nullptr;
    uint64_t size_ = 0;
    bool creator_ = false;
    std::string name_;
    std::string lastError_;
#if defined(HFT_WINDOWS)
    HANDLE handle_ = nullptr;
#else
    int fd_ = -1;
    std::string shmName_;
#endif
};

// Typed shared-memory region holding a single POD/trivially-constructible
// object T (typically a struct bundling several ring queues).
//
// `create()` performs placement-new so that atomics and counters start from a
// well defined state. `open()` assumes someone else already did that.
// Note: T may legitimately contain std::atomic members (they are fine inside
// shared memory on every platform we target), so we only forbid polymorphism
// and virtual bases here.
template <typename T>
class ShmRegion {
    static_assert(!std::is_polymorphic<T>::value,
                  "shared region payload must not be polymorphic (no vtable)");

public:
    ShmRegion() = default;

    bool create(const std::string& name) {
        if (!mem_.create(name, sizeof(T))) return false;
        T* obj = new (mem_.address()) T(); // zero-initialise / value-initialise
        obj_ = obj;
        return true;
    }

    bool open(const std::string& name) {
        if (!mem_.open(name, sizeof(T))) return false;
        obj_ = mem_.as<T>();
        return true;
    }

    // Opens when it exists, otherwise creates it. Always returns a valid object
    // or false.
    bool openOrCreate(const std::string& name) {
        if (open(name)) return true;
        return create(name);
    }

    T* get() const { return obj_; }
    T* operator->() const { return obj_; }
    T& operator*() const { return *obj_; }
    bool valid() const { return obj_ != nullptr; }
    bool isCreator() const { return mem_.isCreator(); }
    const std::string& lastError() const { return mem_.lastError(); }
    void close() { obj_ = nullptr; mem_.close(); }

private:
    SharedMemory mem_;
    T* obj_ = nullptr;
};

} // namespace hft

#endif // HFT_SHM_H
