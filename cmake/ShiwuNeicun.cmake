# Detours can allocate and free these two records while other threads are
# suspended. Use virtual memory so a suspended owner of the process heap
# cannot block the transaction. Keep the downloaded sources untouched.
set(detours_original "${detours_src}/detours.cpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${detours_original}")
file(READ "${detours_original}" detours_text)
if(NOT detours_text MATCHES "struct DetourThread[\r\n]+[{]" OR
   NOT detours_text MATCHES "struct DetourOperation[\r\n]+[{]")
    message(FATAL_ERROR "The selected Detours source does not match the transaction allocator patch.")
endif()
set(detours_memory [=[
struct DmDetourTransactionMemory
{
    static void* operator new(size_t bytes) noexcept
    {
        return VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    }

    static void operator delete(void* memory) noexcept
    {
        if (memory) VirtualFree(memory, 0, MEM_RELEASE);
    }
};
]=])
string(REPLACE "struct DetourThread" "${detours_memory}\nstruct DetourThread : DmDetourTransactionMemory"
    detours_text "${detours_text}")
string(REPLACE "struct DetourOperation" "struct DetourOperation : DmDetourTransactionMemory"
    detours_text "${detours_text}")
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/DetoursShengcheng.cpp" CONTENT "${detours_text}" @ONLY)
