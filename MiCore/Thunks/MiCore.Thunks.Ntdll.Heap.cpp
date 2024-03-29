#ifdef _KERNEL_MODE

#include "MiCore.Thunks.Ntdll.Heap.Private.h"
#include "MiCore/MiCore.SystemEnvironmentBlock.Private.h"
#include "MiCore/MiCore.Utility.h"


#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, MI_NAME(RtlCreateHeap))
#pragma alloc_text(PAGE, MI_NAME(RtlDestroyHeap))
#endif


EXTERN_C_START
namespace Mi
{
    VEIL_DECLARE_STRUCT_ALIGN(HEAP_LOCK, 8)
    {
        union {
            ERESOURCE  Resource;    // PagedPool
            KSPIN_LOCK SpinLock;    // NonPagedPool
        } Lock;
    };

    VEIL_DECLARE_STRUCT_ALIGN(HEAP_ENTRY, 8)
    {
        LIST_ENTRY  Entry;
        SIZE_T      Size;
        PVOID       UserData;

        // This field:
        //     Normal: UserData -> Buffer
        // Large-Page: Empty
    };

    VEIL_DECLARE_STRUCT_ALIGN(HEAP, 8)
    {
        ULONG Index;

        ULONG Tag;
        ULONG Type;
        ULONG Priority;
        ULONG Flags;

        LIST_ENTRY Blocks;
        HEAP_LOCK* LockVariable;

        PVOID(NTAPI* Allocate)(
            _In_ __drv_strictTypeMatch(__drv_typeExpr) POOL_TYPE PoolType,
            _In_ SIZE_T NumberOfBytes,
            _In_ ULONG  Tag);

        VOID (NTAPI* Destroy)(
            _Pre_notnull_ __drv_freesMem(Mem) PVOID Buffer,
            _In_ ULONG Tag);

        // HEAP_LOCK Lock
    };


#define MI_HEAP_CREATE_VALID_MASK  (HEAP_NO_SERIALIZE           | \
                                    HEAP_GROWABLE               | \
                                    HEAP_GENERATE_EXCEPTIONS    | \
                                    HEAP_ZERO_MEMORY            | \
                                    HEAP_CREATE_SEGMENT_HEAP)

#define HEAP_LOCK_USER_ALLOCATED    0x80000000

    PVOID NTAPI RtlGetDefaultHeap();

#ifdef _X86_
    _VEIL_DECLARE_ALTERNATE_NAME(RtlGetDefaultHeap@0, _Mi_RtlGetDefaultHeap@0);
#else
    _VEIL_DECLARE_ALTERNATE_NAME(RtlGetDefaultHeap, _Mi_RtlGetDefaultHeap);
#endif


    // Private functions

    static VOID NTAPI MI_NAME_PRIVATE(RtlInitializeHeapLock)(_Inout_ PHEAP Heap)
    {
        if (Heap->Type == PagedPool) {
            (VOID)ExInitializeResourceLite(&Heap->LockVariable->Lock.Resource);
        }
        else {
            KeInitializeSpinLock(&Heap->LockVariable->Lock.SpinLock);
        }
    }

    static VOID NTAPI MI_NAME_PRIVATE(RtlDestroyHeapLock)(_Inout_ PHEAP Heap)
    {
        if (Heap->Type == PagedPool) {
            (void)ExDeleteResourceLite(&Heap->LockVariable->Lock.Resource);
        }
    }

    static VOID NTAPI MI_NAME_PRIVATE(RtlAcquireHeapLockExclusive)(_Inout_ PHEAP Heap, _Inout_ PKLOCK_QUEUE_HANDLE LockHandle)
    {
        if (Heap->Type == PagedPool) {
            ExEnterCriticalRegionAndAcquireResourceExclusive(&Heap->LockVariable->Lock.Resource);
        }
        else {
            KeAcquireInStackQueuedSpinLock(&Heap->LockVariable->Lock.SpinLock, LockHandle);
        }
    }

    static VOID NTAPI MI_NAME_PRIVATE(RtlAcquireHeapLockShared)(_Inout_ PHEAP Heap, _Inout_ PKLOCK_QUEUE_HANDLE LockHandle)
    {
        if (Heap->Type == PagedPool) {
            ExEnterCriticalRegionAndAcquireResourceShared(&Heap->LockVariable->Lock.Resource);
        }
        else {
            KeAcquireInStackQueuedSpinLock(&Heap->LockVariable->Lock.SpinLock, LockHandle);
        }
    }

    static VOID NTAPI MI_NAME_PRIVATE(RtlReleaseHeapLock)(_Inout_ PHEAP Heap, _Inout_ PKLOCK_QUEUE_HANDLE LockHandle)
    {
        if (Heap->Type == PagedPool) {
            ExReleaseResourceAndLeaveCriticalRegion(&Heap->LockVariable->Lock.Resource);
        }
        else {
            KeReleaseInStackQueuedSpinLock(LockHandle);
        }
    }

    static NTSTATUS NTAPI MI_NAME_PRIVATE(RtlAppendHeap)(_In_ PHEAP Heap)
    {
        NTSTATUS Status = STATUS_SUCCESS;

        const auto Peb = MI_NAME_PRIVATE(RtlGetCurrentPeb)();
        MI_NAME_PRIVATE(RtlAcquirePebLock)();
        __try {
            //
            //  If the processes heap list is already full then we'll
            //  double the size of the heap list for the process
            //

            if (Peb->NumberOfHeaps == Peb->MaximumNumberOfHeaps) {
                Peb->MaximumNumberOfHeaps *= 2;

                //
                //  Copy over the old buffer to the new buffer
                //

                auto NewList = static_cast<PHEAP*>(RtlAllocateHeap(RtlGetDefaultHeap(), 0,
                    Peb->MaximumNumberOfHeaps * sizeof(PHEAP)));
                if (NewList == nullptr) {
                    Peb->MaximumNumberOfHeaps = Peb->NumberOfHeaps;

                    Status = STATUS_INSUFFICIENT_RESOURCES;
                    __leave;
                }

                RtlCopyMemory(NewList,
                    Peb->ProcessHeaps,
                    Peb->NumberOfHeaps * sizeof(PHEAP));

                //
                //  Check if we should free the previous heap list buffer
                //

                if (Peb->ProcessHeaps != Add2Ptr(Peb, sizeof(KPEB))) {
                    RtlFreeHeap(RtlGetDefaultHeap(), 0, Peb->ProcessHeaps);
                }

                //
                //  Set the new list
                //

                Peb->ProcessHeaps = reinterpret_cast<PVOID*>(NewList);
            }

            //
            //  Add the input heap to the next free heap list slot, and note that
            //  the processes heap list index is really one beyond the actualy
            //  index used to get the processes heap
            //

            Peb->ProcessHeaps[Peb->NumberOfHeaps++] = FastEncodePointer(Heap);
            Heap->Index = Peb->NumberOfHeaps;
        }
        __finally {
            MI_NAME_PRIVATE(RtlReleasePebLock)();
        }

        return Status;
    }

    static VOID NTAPI MI_NAME_PRIVATE(RtlRemoveHeap)(_In_ PHEAP Heap)
    {
        const auto Peb = MI_NAME_PRIVATE(RtlGetCurrentPeb)();
        MI_NAME_PRIVATE(RtlAcquirePebLock)();
        __try {

            //
            //  We only want to the the work if the current process actually has some
            //  heaps, the index stored in the heap is within the range for active
            //  heaps.
            //

            if ((Peb->NumberOfHeaps != 0) &&
                (Heap->Index != 0) &&
                (Heap->Index <= Peb->NumberOfHeaps)) {

                //
                //  Establish a pointer into the array of process heaps at the
                //  current heap location and one beyond
                //

                auto ThisLocation = reinterpret_cast<PHEAP*>(&Peb->ProcessHeaps[Heap->Index - 1]);
                auto NextLocation = ThisLocation + 1;

                //
                //  Calculate the number of heaps that exist beyond the current
                //  heap in the array including the current heap location
                //

                auto Leftover = Peb->NumberOfHeaps - (Heap->Index - 1);

                //
                //  For every heap beyond the current one that we are removing
                //  we'll move that heap down to the previous index.
                //

                while (--Leftover) {

                    //
                    //  Copy the heap process array entry of the next entry to
                    //  the current entry, and move NextLocation to the next next entry
                    //

                    *ThisLocation = *NextLocation++;

                    //
                    //  Assign the moved heap its new heap index
                    //

                    (*ThisLocation)->Index -= 1;

                    //
                    //  Move on to the next heap entry
                    //

                    ThisLocation += 1;
                }

                //
                //  Zero out the last process heap pointer, update the count, and
                //  make the heap we just removed realize it has been removed by
                //  zeroing out its process heap list index
                //

                Heap->Index = 0;
                Peb->ProcessHeaps[--Peb->NumberOfHeaps] = nullptr;
            }
        }
        __finally {
            MI_NAME_PRIVATE(RtlReleasePebLock)();
        }
    }


    // Public  functions

    PVOID NTAPI MI_NAME(RtlGetDefaultHeap)()
    {
        return MI_NAME_PRIVATE(RtlGetCurrentPeb)()->DefaultHeap;
    }
    MI_IAT_SYMBOL(RtlGetDefaultHeap, 0);

    _IRQL_requires_max_(APC_LEVEL)
    _Must_inspect_result_
    PVOID NTAPI MI_NAME(RtlCreateHeap)(
        _In_     ULONG  Flags,
        _In_opt_ PVOID  HeapBase,
        _In_opt_ SIZE_T ReserveSize,
        _In_opt_ SIZE_T CommitSize,
        _In_opt_ PVOID  Lock,
        _When_((Flags & 0x100) != 0,
            _In_reads_bytes_opt_(sizeof(RTL_SEGMENT_HEAP_PARAMETERS)))
        _When_((Flags & 0x100) == 0,
            _In_reads_bytes_opt_(sizeof(RTL_HEAP_PARAMETERS)))
        PRTL_HEAP_PARAMETERS Parameters
        )
    {
        PAGED_CODE()

        UNREFERENCED_PARAMETER(ReserveSize);
        UNREFERENCED_PARAMETER(CommitSize);

        NTSTATUS Status;
        PHEAP    Heap       = nullptr;
        PVOID    HeapHandle = nullptr;

        do {
            if (HeapBase) {
                Status = STATUS_NOT_SUPPORTED;

                KdBreakPointWithStatus(Status);
                break;
            }

            if (BooleanFlagOn(Flags, ~MI_HEAP_CREATE_VALID_MASK)) {
                Status = STATUS_NOT_SUPPORTED;

                KdBreakPointWithStatus(Status);
                break;
            }

            if (Parameters != nullptr) {
                if (BooleanFlagOn(Flags, HEAP_CREATE_SEGMENT_HEAP) == FALSE) {
                    Status = STATUS_NOT_SUPPORTED;

                    KdBreakPointWithStatus(Status);
                    break;
                }
            }

            Flags &= (HEAP_NO_SERIALIZE         | \
                      HEAP_GENERATE_EXCEPTIONS  | \
                      HEAP_ZERO_MEMORY);

            auto SizeOfHeap = sizeof(HEAP);

            if (!BooleanFlagOn(Flags, HEAP_NO_SERIALIZE)) {
                if (Lock != nullptr) {
                    SetFlag(Flags, HEAP_LOCK_USER_ALLOCATED);
                }
                else {
                    SizeOfHeap += sizeof(HEAP_LOCK);
                }
            }
            else if (ARGUMENT_PRESENT(Lock)) {
                Status = STATUS_INVALID_PARAMETER;
                break;
            }

            Heap = static_cast<PHEAP>(ExAllocatePoolZero(NonPagedPool, SizeOfHeap, MI_TAG));
            if (Heap == nullptr) {
                Status = STATUS_INSUFFICIENT_RESOURCES;
                break;
            }

        #pragma warning( suppress : 4996 28751 )
            Heap->Allocate  = ExAllocatePoolWithTag;
            Heap->Destroy   = ExFreePoolWithTag;
            Heap->Tag       = MI_TAG;
            Heap->Type      = NonPagedPool;
            Heap->Priority  = NormalPoolPriority;
            Heap->Flags     = Flags;
            Heap->LockVariable = static_cast<PHEAP_LOCK>(Lock);

            InitializeListHead(&Heap->Blocks);

            if (Parameters) {
                const auto SegmentHeapParameters = reinterpret_cast<PRTL_SEGMENT_HEAP_PARAMETERS>(Parameters);
                if (!BooleanFlagOn(SegmentHeapParameters->MemorySource.MemoryTypeMask, MemoryTypeNonPaged)) {
                    Heap->Type = PagedPool;
                }
                Heap->Tag = static_cast<ULONG>(SegmentHeapParameters->MemorySource.Reserved[1]);
            }

            if (!BooleanFlagOn(Heap->Flags, HEAP_LOCK_USER_ALLOCATED)) {
                Heap->LockVariable = static_cast<PHEAP_LOCK>(Add2Ptr(Heap, sizeof(HEAP)));
                MI_NAME_PRIVATE(RtlInitializeHeapLock)(Heap);
            }

            Status = MI_NAME_PRIVATE(RtlAppendHeap)(Heap);
            if (!NT_SUCCESS(Status)) {
                break;
            }

            HeapHandle = FastEncodePointer(Heap);

        } while (false);

        if (!NT_SUCCESS(Status)) {
            if (Heap) {
                if (!BooleanFlagOn(Heap->Flags, HEAP_LOCK_USER_ALLOCATED)) {
                    MI_NAME_PRIVATE(RtlDestroyHeapLock)(Heap);
                }
                ExFreePoolWithTag(Heap, MI_TAG);
            }
        }

        return HeapHandle;
    }
    MI_IAT_SYMBOL(RtlCreateHeap, 24);

    _IRQL_requires_max_(APC_LEVEL)
    PVOID NTAPI MI_NAME(RtlDestroyHeap)(
        _In_ _Post_invalid_ PVOID HeapHandle
        )
    {
        PAGED_CODE()

        do {
            if (HeapHandle == nullptr) {
                break;
            }

            const auto Heap = static_cast<PHEAP>(FastDecodePointer(HeapHandle));
            if (Heap == nullptr) {
                break;
            }

            //
            //  For every allocation we remove it from the list and free the vm
            //

            BOOLEAN LockAcquired = FALSE;
            KLOCK_QUEUE_HANDLE LockHandle{};

            if (!BooleanFlagOn(Heap->Flags, HEAP_NO_SERIALIZE)) {
                MI_NAME_PRIVATE(RtlAcquireHeapLockExclusive)(Heap, &LockHandle);
                LockAcquired = TRUE;
            }
            __try {
                while (!IsListEmpty(&Heap->Blocks)) {
                    const auto Entry = RemoveTailList(&Heap->Blocks);
                    const auto Block = CONTAINING_RECORD(Entry, HEAP_ENTRY, Entry);

                    if (Block->Size >= PAGE_SIZE) {
                        Heap->Destroy(Block->UserData, Heap->Tag);
                    }
                    Heap->Destroy(Block, Heap->Tag);
                }
            }
            __finally {
                if (LockAcquired) {
                    MI_NAME_PRIVATE(RtlReleaseHeapLock)(Heap, &LockHandle);
                }
            }

            //
            //  We need to remove this heap from the process heap list
            //

            MI_NAME_PRIVATE(RtlRemoveHeap)(Heap);

            //
            //  If the heap is serialized, delete the lock created
            //  by RtlCreateHeap.
            //
            if (!BooleanFlagOn(Heap->Flags, HEAP_NO_SERIALIZE)) {
                if (!BooleanFlagOn(Heap->Flags, HEAP_LOCK_USER_ALLOCATED)) {
                    MI_NAME_PRIVATE(RtlDestroyHeapLock)(Heap);
                }
                Heap->LockVariable = nullptr;
            }

            ExFreePoolWithTag(Heap, MI_TAG);

        } while (false);

        return nullptr;
    }
    MI_IAT_SYMBOL(RtlDestroyHeap, 4);

    _IRQL_requires_max_(DISPATCH_LEVEL)
    _Must_inspect_result_
    _Ret_maybenull_
    _Post_writable_byte_size_(Size)
    PVOID NTAPI MI_NAME(RtlAllocateHeap)(
        _In_ PVOID HeapHandle,
        _In_opt_ ULONG Flags,
        _In_ SIZE_T Size
        )
    {
        if (HeapHandle == nullptr) {
            return nullptr;
        }

        const auto Heap = static_cast<PHEAP>(FastDecodePointer(HeapHandle));
        if (Heap == nullptr) {
            return nullptr;
        }

        SetFlag(Flags, Heap->Flags);

        if (Size > MAXINT_PTR) {
            return nullptr;
        }

        auto BlockSize = sizeof(HEAP_ENTRY);
        if (Size < PAGE_SIZE) {
            BlockSize += (Size ? Size : 1);
        }

        NTSTATUS Status      = STATUS_SUCCESS;
        PVOID    BaseAddress = nullptr;

        do {
            const auto Block = static_cast<PHEAP_ENTRY>(Heap->Allocate(static_cast<POOL_TYPE>(Heap->Type), BlockSize, Heap->Tag));
            if (Block == nullptr) {
                Status = STATUS_NO_MEMORY;
                break;
            }
            InitializeListHead(&Block->Entry);

            Block->Size = Size;

            if (Size < PAGE_SIZE) {
                Block->UserData = Add2Ptr(Block, sizeof(HEAP_ENTRY));
            }
            else {
                Block->UserData = Heap->Allocate(static_cast<POOL_TYPE>(Heap->Type), Size, Heap->Tag);
            }

            if (Block->UserData == nullptr) {
                Heap->Destroy(Block, Heap->Tag);

                Status = STATUS_NO_MEMORY;
                break;
            }

            BaseAddress = Block->UserData;

            //
            //  If the flags indicate that we should zero memory then do it now
            //

            if (Flags & HEAP_ZERO_MEMORY) {
                RtlZeroMemory(BaseAddress, Size);
            }

            BOOLEAN LockAcquired = FALSE;
            KLOCK_QUEUE_HANDLE LockHandle{};

            if (!BooleanFlagOn(Flags, HEAP_NO_SERIALIZE)) {
                MI_NAME_PRIVATE(RtlAcquireHeapLockExclusive)(Heap, &LockHandle);
                LockAcquired = TRUE;
            }
            __try {
                InsertHeadList(&Heap->Blocks, &Block->Entry);
            }
            __finally {
                if (LockAcquired) {
                    MI_NAME_PRIVATE(RtlReleaseHeapLock)(Heap, &LockHandle);
                }
            }

        } while (false);

        if (Status == STATUS_NO_MEMORY) {
            if (Flags & HEAP_GENERATE_EXCEPTIONS) {
                EXCEPTION_RECORD ExceptionRecord{};

                ExceptionRecord.ExceptionCode           = STATUS_NO_MEMORY;
                ExceptionRecord.ExceptionAddress        = _ReturnAddress();
                ExceptionRecord.NumberParameters        = 2;
                ExceptionRecord.ExceptionFlags          = 0;
                ExceptionRecord.ExceptionInformation[0] = Size;
                ExceptionRecord.ExceptionInformation[1] = BlockSize;

                RtlRaiseException(&ExceptionRecord);
            }
        }

        return BaseAddress;
    }
    MI_IAT_SYMBOL(RtlAllocateHeap, 12);

    _IRQL_requires_max_(DISPATCH_LEVEL)
    _Must_inspect_result_
    _Ret_maybenull_
    _Post_writable_byte_size_(Size)
    PVOID NTAPI MI_NAME(RtlReAllocateHeap)(
        _In_ PVOID HeapHandle,
        _In_ ULONG Flags,
        _Frees_ptr_opt_ PVOID BaseAddress,
        _In_ SIZE_T Size
        )
    {
        if (HeapHandle == nullptr) {
            return nullptr;
        }

        const auto Heap = static_cast<PHEAP>(FastDecodePointer(HeapHandle));
        if (Heap == nullptr) {
            return nullptr;
        }

        SetFlag(Flags, Heap->Flags);

        if (BooleanFlagOn(Flags, HEAP_REALLOC_IN_PLACE_ONLY)) {
            return nullptr;
        }

        if (Size > MAXINT_PTR) {
            return nullptr;
        }

        PVOID NewAddress;

        do {
            SIZE_T OldSize = 0;

            if (BaseAddress) {
            #pragma warning(suppress: 6001)
                OldSize = RtlSizeHeap(HeapHandle, 0, BaseAddress);
            }

            //
            //  Allocate from the heap space for the reallocation
            //

            NewAddress = RtlAllocateHeap(HeapHandle, Flags & ~HEAP_ZERO_MEMORY, (Size ? Size : 1));
            if (NewAddress == nullptr) {
                break;
            }

            //
            //  Copy over the user's data area to the new block
            //

            if (BaseAddress) {
                RtlCopyMemory(NewAddress, BaseAddress, Size < OldSize ? Size : OldSize);
            }

            //
            //  Check if we grew the block and we should zero
            //  the remaining part.
            //

            if (Size > OldSize && (Flags & HEAP_ZERO_MEMORY)) {
                RtlZeroMemory(static_cast<PCHAR>(NewAddress) + OldSize, Size - OldSize);
            }

            //
            //  Release the old block
            //

            RtlFreeHeap(HeapHandle, Flags, BaseAddress);

        } while (false);

        return NewAddress;
    }
    MI_IAT_SYMBOL(RtlReAllocateHeap, 16);

    _IRQL_requires_max_(DISPATCH_LEVEL)
    _Success_(return != 0)
    LOGICAL NTAPI MI_NAME(RtlFreeHeap)(
        _In_ PVOID HeapHandle,
        _In_opt_ ULONG Flags,
        _Frees_ptr_opt_ PVOID BaseAddress
        )
    {
        LOGICAL Result = FALSE;

        if (BaseAddress == nullptr) {
            return TRUE;
        }

        if (HeapHandle == nullptr) {
            return Result;
        }

        const auto Heap = static_cast<PHEAP>(FastDecodePointer(HeapHandle));
        if (Heap == nullptr) {
            return Result;
        }

        SetFlag(Flags, Heap->Flags);

        BOOLEAN LockAcquired = FALSE;
        KLOCK_QUEUE_HANDLE LockHandle{};

        if (!BooleanFlagOn(Flags, HEAP_NO_SERIALIZE)) {
            MI_NAME_PRIVATE(RtlAcquireHeapLockExclusive)(Heap, &LockHandle);
            LockAcquired = TRUE;
        }
        __try {
            for (auto Entry = Heap->Blocks.Flink; Entry != &Heap->Blocks; Entry = Entry->Flink) {
                const auto Block = CONTAINING_RECORD(Entry, HEAP_ENTRY, Entry);
                if (Block->UserData == BaseAddress) {
                    RemoveEntryList(Entry);

                    if (Block->Size >= PAGE_SIZE) {
                        Heap->Destroy(Block->UserData, Heap->Tag);
                    }
                    Heap->Destroy(Block, Heap->Tag);

                    Result = TRUE;
                    break;
                }
            }
        }
        __finally {
            if (LockAcquired) {
                MI_NAME_PRIVATE(RtlReleaseHeapLock)(Heap, &LockHandle);
            }
        }

        return Result;
    }
    MI_IAT_SYMBOL(RtlFreeHeap, 12);

    _IRQL_requires_max_(DISPATCH_LEVEL)
    SIZE_T NTAPI MI_NAME(RtlSizeHeap)(
        _In_ PVOID HeapHandle,
        _In_ ULONG Flags,
        _In_ PVOID BaseAddress
        )
    {
        SIZE_T Result = static_cast<SIZE_T>(-1);

        if (BaseAddress == nullptr) {
            return 0;
        }

        if (HeapHandle == nullptr) {
            return Result;
        }

        const auto Heap = static_cast<PHEAP>(FastDecodePointer(HeapHandle));
        if (Heap == nullptr) {
            return Result;
        }

        SetFlag(Flags, Heap->Flags);

        BOOLEAN LockAcquired = FALSE;
        KLOCK_QUEUE_HANDLE LockHandle{};

        if (!BooleanFlagOn(Flags, HEAP_NO_SERIALIZE)) {
            MI_NAME_PRIVATE(RtlAcquireHeapLockShared)(Heap, &LockHandle);
            LockAcquired = TRUE;
        }
        __try {
            for (auto Entry = Heap->Blocks.Flink; Entry != &Heap->Blocks; Entry = Entry->Flink) {
                const auto Block = CONTAINING_RECORD(Entry, HEAP_ENTRY, Entry);
                if (Block->UserData == BaseAddress) {
                    Result = Block->Size;
                    break;
                }
            }
        }
        __finally {
            if (LockAcquired) {
                MI_NAME_PRIVATE(RtlReleaseHeapLock)(Heap, &LockHandle);
            }
        }

        return Result;
    }
    MI_IAT_SYMBOL(RtlSizeHeap, 12);

    _IRQL_requires_max_(DISPATCH_LEVEL)
    NTSTATUS NTAPI MI_NAME(RtlZeroHeap)(
        _In_ PVOID HeapHandle,
        _In_ ULONG Flags
        )
    {
        UNREFERENCED_PARAMETER(HeapHandle);
        UNREFERENCED_PARAMETER(Flags);

        return STATUS_SUCCESS;
    }
    MI_IAT_SYMBOL(RtlZeroHeap, 12);


}
EXTERN_C_END


#endif // _KERNEL_MODE
