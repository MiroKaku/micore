#include "MiCore.Utility.h"


namespace Mi
{
#ifdef _KERNEL_MODE
    PVOID MICORE_API GetLoadedModuleBase(_In_ PCUNICODE_STRING ModuleName)
    {
        PVOID ModuleBase = nullptr;

        /* Lock the list */
        ExEnterCriticalRegionAndAcquireResourceShared(PsLoadedModuleResource);
        {
            /* Loop the loaded module list */
            for (LIST_ENTRY const* NextEntry = PsLoadedModuleList->Flink; NextEntry != PsLoadedModuleList;)
            {
                /* Get the entry */
                const auto LdrEntry = CONTAINING_RECORD(NextEntry, KLDR_DATA_TABLE_ENTRY, InLoadOrderLinks);

                /* Check if it's the module */
                if (RtlEqualUnicodeString(ModuleName, &LdrEntry->BaseDllName, TRUE))
                {
                    /* Found it */
                    ModuleBase = LdrEntry->DllBase;
                    break;
                }

                /* Keep looping */
                NextEntry = NextEntry->Flink;
            }
        }
        /* Release the lock */
        ExReleaseResourceAndLeaveCriticalRegion(PsLoadedModuleResource);

        return ModuleBase;
    }
#endif
}
