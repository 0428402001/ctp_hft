// SPDX-License-Identifier: MIT
#include "hft/shm.h"

#include <cstdio>

#if defined(HFT_WINDOWS)
namespace {
// Best effort: keep the mapped ring pages resident so a process that has been
// idle for a while does not take soft page faults on the first tick after a
// quiet stretch (the classic "first message of the session is the slowest"
// effect). Raising the working-set ceiling first lets VirtualLock cover the
// ~11 MB of channel rings under the default quota. Never fatal.
void tryLockPages(void* addr, uint64_t size) {
    static const bool quotaRaised = [] {
        return ::SetProcessWorkingSetSize(::GetCurrentProcess(),
                                          static_cast<SIZE_T>(32) * 1024 * 1024,
                                          static_cast<SIZE_T>(256) * 1024 * 1024) != 0;
    }();
    (void)quotaRaised;
    if (!::VirtualLock(addr, static_cast<SIZE_T>(size))) {
        std::fprintf(stderr,
                     "shm: VirtualLock(%llu bytes) failed (err=%lu) - continuing unlocked\n",
                     static_cast<unsigned long long>(size),
                     static_cast<unsigned long>(::GetLastError()));
    }
}
} // namespace
#endif

namespace hft {

void SharedMemory::setError(const char* what) {
    lastError_ = what;
#if defined(HFT_WINDOWS)
    lastError_ += " (GetLastError=" + std::to_string(static_cast<unsigned long>(::GetLastError())) + ")";
#else
    lastError_ += " (errno=" + std::to_string(errno) + ")";
#endif
}

void SharedMemory::close() {
    if (addr_) {
#if defined(HFT_WINDOWS)
        ::UnmapViewOfFile(addr_);
#else
        ::munmap(addr_, static_cast<size_t>(size_));
#endif
        addr_ = nullptr;
    }
#if defined(HFT_WINDOWS)
    if (handle_) {
        ::CloseHandle(handle_);
        handle_ = nullptr;
    }
#else
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    if (creator_ && !shmName_.empty()) {
        ::shm_unlink(shmName_.c_str());
        shmName_.clear();
    }
#endif
    size_ = 0;
    creator_ = false;
}

bool SharedMemory::create(const std::string& name, uint64_t size) {
    close();
    name_ = name;
    size_ = size;

#if defined(HFT_WINDOWS)
    // "Local\\" keeps the object in the current session namespace, which is what
    // we want for co-operating processes started from the same console.
    std::string sysName = "Local\\" + name;
    DWORD hi = static_cast<DWORD>(size >> 32);
    DWORD lo = static_cast<DWORD>(size & 0xFFFFFFFFull);
    handle_ = ::CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, hi, lo,
                                   sysName.c_str());
    if (!handle_) {
        setError("CreateFileMapping failed");
        return false;
    }
    creator_ = (::GetLastError() != ERROR_ALREADY_EXISTS);
    addr_ = ::MapViewOfFile(handle_, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0);
    if (!addr_) {
        setError("MapViewOfFile failed");
        ::CloseHandle(handle_);
        handle_ = nullptr;
        return false;
    }
    tryLockPages(addr_, size_);
#else
    shmName_ = "/" + name;
    fd_ = ::shm_open(shmName_.c_str(), O_CREAT | O_EXCL | O_RDWR, 0666);
    if (fd_ < 0 && errno == EEXIST) {
        fd_ = ::shm_open(shmName_.c_str(), O_RDWR, 0666);
        creator_ = false;
    } else {
        creator_ = true;
    }
    if (fd_ < 0) {
        setError("shm_open failed");
        return false;
    }
    if (creator_ && ::ftruncate(fd_, static_cast<off_t>(size)) != 0) {
        setError("ftruncate failed");
        ::close(fd_);
        fd_ = -1;
        return false;
    }
    addr_ = ::mmap(nullptr, static_cast<size_t>(size), PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (addr_ == MAP_FAILED) {
        setError("mmap failed");
        addr_ = nullptr;
        ::close(fd_);
        fd_ = -1;
        return false;
    }
#endif
    return true;
}

bool SharedMemory::open(const std::string& name, uint64_t size) {
    close();
    name_ = name;
    size_ = size;

#if defined(HFT_WINDOWS)
    std::string sysName = "Local\\" + name;
    handle_ = ::OpenFileMappingA(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, sysName.c_str());
    if (!handle_) {
        setError("OpenFileMapping failed");
        return false;
    }
    addr_ = ::MapViewOfFile(handle_, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0);
    if (!addr_) {
        setError("MapViewOfFile failed");
        ::CloseHandle(handle_);
        handle_ = nullptr;
        return false;
    }
    tryLockPages(addr_, size_);
#else
    shmName_ = "/" + name;
    fd_ = ::shm_open(shmName_.c_str(), O_RDWR, 0666);
    if (fd_ < 0) {
        setError("shm_open failed (not created yet?)");
        return false;
    }
    addr_ = ::mmap(nullptr, static_cast<size_t>(size), PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (addr_ == MAP_FAILED) {
        setError("mmap failed");
        addr_ = nullptr;
        ::close(fd_);
        fd_ = -1;
        return false;
    }
#endif
    creator_ = false;
    return true;
}

} // namespace hft
