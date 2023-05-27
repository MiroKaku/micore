#pragma once


namespace Mi::Util
{
    constexpr int MAXIMUM_POINTER_SHIFT = sizeof(uintptr_t) * 8;

    inline uintptr_t RotatePointerValue(uintptr_t Value, int const Shift) noexcept
    {
    #if defined(_WIN64)
        return RotateRight64(Value, Shift);
    #else
        return RotateRight32(Value, Shift);
    #endif
    }

    template <typename T>
    T FastDecodePointer(T const Ptr) noexcept
    {
        return reinterpret_cast<T>(RotatePointerValue(reinterpret_cast<uintptr_t>(Ptr) ^ __security_cookie,
            static_cast<int>(__security_cookie % MAXIMUM_POINTER_SHIFT)));
    }

    template <typename T>
    T FastEncodePointer(T const Ptr) noexcept
    {
        return reinterpret_cast<T>(RotatePointerValue(reinterpret_cast<uintptr_t>(Ptr),
            MAXIMUM_POINTER_SHIFT - static_cast<int>(__security_cookie % MAXIMUM_POINTER_SHIFT)) ^ __security_cookie);
    }

    constexpr size_t Fnv1aHash(_In_ const char* Buffer, _In_ const size_t Count) noexcept
    {
    #if defined(_WIN64)
        constexpr size_t FnvOffsetBasis = 14695981039346656037ULL;
        constexpr size_t FnvPrime       = 1099511628211ULL;
    #else
        constexpr size_t FnvOffsetBasis = 2166136261U;
        constexpr size_t FnvPrime       = 16777619U;
    #endif

        auto Value = FnvOffsetBasis;
        for (size_t Idx = 0; Idx < Count; ++Idx) {
            Value ^= static_cast<size_t>(Buffer[Idx]);
            Value *= FnvPrime;
        }
        return Value;
    }

    NTSTATUS ImageEnumerateExports(
        _In_ void* BaseOfImage,
        _In_ bool(CALLBACK *Callback)(uint32_t Ordinal, const char* Name, const void* Address, void* Context),
        _In_opt_ void* Context
    );

}