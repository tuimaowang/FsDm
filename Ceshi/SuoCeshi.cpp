#include "Xieyi.h"
#include <detours.h>
#include <cstdio>
#include <new>

namespace {
SRWLOCK allocatorLock = SRWLOCK_INIT;
}

// 模拟被挂起线程持有内存分配锁，检查安装拦截时不会死锁。
void* operator new(size_t size) {
    AcquireSRWLockExclusive(&allocatorLock);
    void* memory = HeapAlloc(GetProcessHeap(), 0, size ? size : 1);
    ReleaseSRWLockExclusive(&allocatorLock);
    if (!memory) throw std::bad_alloc{};
    return memory;
}

void operator delete(void* memory) noexcept {
    if (!memory) return;
    AcquireSRWLockExclusive(&allocatorLock);
    HeapFree(GetProcessHeap(), 0, memory);
    ReleaseSRWLockExclusive(&allocatorLock);
}

void operator delete(void* memory, size_t) noexcept {
    ::operator delete(memory);
}

namespace {
__declspec(noinline) LONG WINAPI Target(volatile LONG* value) {
    return InterlockedIncrement(value);
}
decltype(&Target) original = Target;

LONG WINAPI Hook(volatile LONG* value) {
    return original(value) + 100;
}

struct Events {
    replica::Handle locked{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    replica::Handle release{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
};

DWORD WINAPI HoldHeap(void* context) {
    auto& events = *static_cast<Events*>(context);
    AcquireSRWLockExclusive(&allocatorLock);
    SetEvent(events.locked.get());
    const auto wait = WaitForSingleObject(events.release.get(), INFINITE);
    ReleaseSRWLockExclusive(&allocatorLock);
    return wait == WAIT_OBJECT_0 ? 0 : 1;
}
} // namespace

int main() {
    Events events;
    if (!events.locked || !events.release) return 1;
    LONG err = DetourTransactionBegin();
    if (err) return 2;
    err = DetourAttach(reinterpret_cast<PVOID*>(&original), reinterpret_cast<PVOID>(Hook));
    if (err) {
        DetourTransactionAbort();
        return 3;
    }
    const replica::Handle thread(CreateThread(nullptr, 0, HoldHeap, &events, 0, nullptr));
    if (thread && WaitForSingleObject(events.locked.get(), 2000) == WAIT_OBJECT_0) {
        err = DetourUpdateThread(thread.get());
    } else {
        err = ERROR_TIMEOUT;
    }
    if (err) DetourTransactionAbort();
    else err = DetourTransactionCommit();
    SetEvent(events.release.get());
    if (!thread || WaitForSingleObject(thread.get(), 2000) != WAIT_OBJECT_0 || err) return 4;
    volatile LONG value = 0;
    const auto entry = &Target;
    if (entry(&value) != 101 || value != 1) return 5;
    std::puts("Detours committed and resumed the thread while it owned the allocator lock.");
    return 0;
}
