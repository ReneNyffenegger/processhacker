/*
 * Process Hacker -
 *   LXSS support helpers
 *
 * Copyright (C) 2019-2021 dmex
 *
 * This file is part of Process Hacker.
 *
 * Process Hacker is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Process Hacker is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Process Hacker.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <ph.h>
#include <wslsup.h>

BOOLEAN NTAPI PhpWslDistributionNamesCallback(
    _In_ HANDLE RootDirectory,
    _In_ PKEY_BASIC_INFORMATION Information,
    _In_opt_ PVOID Context
    )
{
    if (Context)
        PhAddItemList(Context, PhCreateStringEx(Information->Name, Information->NameLength));
    return TRUE;
}

_Success_(return)
BOOLEAN PhGetWslDistributionFromPath(
    _In_ PPH_STRING FileName,
    _Out_opt_ PPH_STRING *LxssDistroName,
    _Out_opt_ PPH_STRING *LxssDistroPath,
    _Out_opt_ PPH_STRING *LxssFileName
    )
{
    static PH_STRINGREF lxssKeyPath = PH_STRINGREF_INIT(L"Software\\Microsoft\\Windows\\CurrentVersion\\Lxss");
    PPH_STRING lxssDistributionName = NULL;
    PPH_STRING lxssDistroPath = NULL;
    PPH_STRING lxssFileName = NULL;
    HANDLE keyHandle;
    ULONG i;

    if (NT_SUCCESS(PhOpenKey(
        &keyHandle,
        KEY_READ,
        PH_KEY_CURRENT_USER,
        &lxssKeyPath,
        0
        )))
    {
        PPH_LIST distributionGuidList;

        distributionGuidList = PhCreateList(1);
        PhEnumerateKey(keyHandle, KeyBasicInformation, PhpWslDistributionNamesCallback, distributionGuidList);

        for (i = 0; i < distributionGuidList->Count; i++)
        {
            HANDLE subKeyHandle;
            PPH_STRING subKeyName;

            subKeyName = distributionGuidList->Items[i];

            if (NT_SUCCESS(PhOpenKey(
                &subKeyHandle,
                KEY_READ,
                keyHandle,
                &subKeyName->sr,
                0
                )))
            {
                PPH_STRING lxssBasePathName = PhQueryRegistryString(subKeyHandle, L"BasePath");

                if (PhStartsWithString(FileName, lxssBasePathName, TRUE))
                {
                    lxssDistributionName = PhQueryRegistryString(subKeyHandle, L"DistributionName");

                    if (LxssDistroPath)
                    {
                        PhSetReference(&lxssDistroPath, lxssBasePathName);
                    }

                    if (LxssFileName)
                    {
                        lxssFileName = PhDuplicateString(FileName);
                        PhSkipStringRef(&lxssFileName->sr, lxssBasePathName->Length);
                        PhSkipStringRef(&lxssFileName->sr, sizeof(L"rootfs"));
                    }
                }

                PhDereferenceObject(lxssBasePathName);
                NtClose(subKeyHandle);
            }

            if (lxssDistributionName)
                break;
        }

        PhDereferenceObjects(distributionGuidList->Items, distributionGuidList->Count);
        PhDereferenceObject(distributionGuidList);

        NtClose(keyHandle);
    }

    if (LxssDistroPath)
        *LxssDistroPath = lxssDistroPath;
    else
    {
        if (lxssDistroPath)
            PhDereferenceObject(lxssDistroPath);
    }

    if (LxssFileName)
    {
        if (lxssFileName)
        {
            for (i = 0; i < lxssFileName->Length / sizeof(WCHAR); i++)
            {
                if (lxssFileName->Buffer[i] == OBJ_NAME_PATH_SEPARATOR) // RtlNtPathSeperatorString
                    lxssFileName->Buffer[i] = L'/'; // RtlAlternateDosPathSeperatorString
            }
        }

        *LxssFileName = lxssFileName;
    }
    else
    {
        if (lxssFileName)
            PhDereferenceObject(lxssFileName);
    }

    if (LxssDistroName)
    {
        *LxssDistroName = lxssDistributionName;
        return TRUE;
    }
    else
    {
        if (lxssDistributionName)
            PhDereferenceObject(lxssDistributionName);
    }

    return FALSE;
}

_Success_(return)
BOOLEAN PhInitializeLxssImageVersionInfo(
    _Inout_ PPH_IMAGE_VERSION_INFO ImageVersionInfo,
    _In_ PPH_STRING FileName
    )
{
    BOOLEAN success = FALSE;
    PPH_STRING lxssCommandResult = NULL;
    PPH_STRING lxssCommandLine = NULL;
    PPH_STRING lxssPackageName = NULL;
    PPH_STRING lxssDistroName;
    PPH_STRING lxssFileName;
    PH_FORMAT format[3];

    if (!PhGetWslDistributionFromPath(FileName, &lxssDistroName, NULL, &lxssFileName))
        return FALSE;
    if (PhIsNullOrEmptyString(lxssDistroName) || PhIsNullOrEmptyString(lxssFileName))
        goto CleanupExit;
    if (PhEqualString2(lxssFileName, L"/init", FALSE))
        goto CleanupExit;

    // "rpm -qf %s --queryformat \"%%{VERSION}|%%{VENDOR}|%%{SUMMARY}\""
    PhInitFormatS(&format[0], L"rpm -qf ");
    PhInitFormatSR(&format[1], lxssFileName->sr);
    PhInitFormatS(&format[2], L" --queryformat \"%%{VERSION}|%%{VENDOR}|%%{SUMMARY}\"");

    lxssCommandLine = PhFormat(
        format,
        RTL_NUMBER_OF(format),
        0x100
        );

    if (PhCreateProcessLxss(
        lxssDistroName,
        lxssCommandLine,
        &lxssCommandResult
        ))
    {
        goto ParseResult;
    }

    PhMoveReference(&lxssCommandLine, PhConcatStrings2(
        L"dpkg -S ",
        PhGetString(lxssFileName)
        ));

    if (PhCreateProcessLxss(
        lxssDistroName,
        lxssCommandLine,
        &lxssCommandResult
        ))
    {
        PH_STRINGREF remainingPart;
        PH_STRINGREF packagePart;

        if (PhSplitStringRefAtChar(&lxssCommandResult->sr, L':', &packagePart, &remainingPart))
        {
            lxssPackageName = PhCreateString2(&packagePart);
        }

        PhDereferenceObject(lxssCommandResult);
    }

    if (PhIsNullOrEmptyString(lxssPackageName))
        goto CleanupExit;

    PhMoveReference(&lxssCommandLine, PhConcatStrings2(
        L"dpkg-query -W -f=${Version}|${Maintainer}|${binary:Summary} ",
        PhGetString(lxssPackageName)
        ));

    if (!PhCreateProcessLxss(
        lxssDistroName,
        lxssCommandLine,
        &lxssCommandResult
        ))
    {
        goto CleanupExit;
    }

ParseResult:
    if (
        !ImageVersionInfo->FileVersion &&
        !ImageVersionInfo->CompanyName &&
        !ImageVersionInfo->FileDescription
        )
    {
        PH_STRINGREF remainingPart;
        PH_STRINGREF companyPart;
        PH_STRINGREF descriptionPart;
        PH_STRINGREF versionPart;

        remainingPart = PhGetStringRef(lxssCommandResult);
        PhSplitStringRefAtChar(&remainingPart, L'|', &versionPart, &remainingPart);
        PhSplitStringRefAtChar(&remainingPart, L'|', &companyPart, &remainingPart);
        PhSplitStringRefAtChar(&remainingPart, L'|', &descriptionPart, &remainingPart);

        if (versionPart.Length != 0)
            ImageVersionInfo->FileVersion = PhCreateString2(&versionPart);
        if (companyPart.Length != 0)
            ImageVersionInfo->CompanyName = PhCreateString2(&companyPart);
        if (descriptionPart.Length != 0)
            ImageVersionInfo->FileDescription = PhCreateString2(&descriptionPart);
    }

    success = TRUE;

CleanupExit:

    if (lxssCommandResult) PhDereferenceObject(lxssCommandResult);
    if (lxssCommandLine) PhDereferenceObject(lxssCommandLine);
    if (lxssPackageName) PhDereferenceObject(lxssPackageName);
    if (lxssDistroName) PhDereferenceObject(lxssDistroName);
    if (lxssFileName) PhDereferenceObject(lxssFileName);

    return success;
}

_Success_(return)
BOOLEAN PhCreateProcessLxss(
    _In_ PPH_STRING LxssDistribution,
    _In_ PPH_STRING LxssCommandLine,
    _Out_ PPH_STRING *Result
    )
{
    NTSTATUS status;
    PPH_STRING lxssOutputString;
    PPH_STRING lxssCommandLine;
    PPH_STRING systemDirectory;
    HANDLE processHandle;
    HANDLE outputReadHandle, outputWriteHandle;
    HANDLE inputReadHandle, inputWriteHandle;
    PROCESS_BASIC_INFORMATION basicInfo;
    STARTUPINFO startupInfo;
    PH_FORMAT format[4];

    // "wsl.exe -u root -d %s -e %s"
    PhInitFormatS(&format[0], L"wsl.exe -u root -d ");
    PhInitFormatSR(&format[1], LxssDistribution->sr);
    PhInitFormatS(&format[2], L" -e ");
    PhInitFormatSR(&format[3], LxssCommandLine->sr);

    lxssCommandLine = PhFormat(
        format,
        RTL_NUMBER_OF(format),
        0x100
        );

    if (systemDirectory = PhGetSystemDirectory())
    {
        PhMoveReference(&lxssCommandLine, PhConcatStrings(
            3,
            PhGetString(systemDirectory),
            L"\\",
            PhGetString(lxssCommandLine)
            ));
        PhDereferenceObject(systemDirectory);
    }

    status = PhCreatePipeEx(
        &outputReadHandle,
        &outputWriteHandle,
        TRUE,
        NULL
        );

    if (!NT_SUCCESS(status))
        return FALSE;

    status = PhCreatePipeEx(
        &inputReadHandle,
        &inputWriteHandle,
        TRUE,
        NULL
        );

    if (!NT_SUCCESS(status))
    {
        NtClose(outputWriteHandle);
        NtClose(outputReadHandle);
        return FALSE;
    }

    memset(&startupInfo, 0, sizeof(STARTUPINFO));
    startupInfo.cb = sizeof(STARTUPINFO);
    startupInfo.dwFlags = STARTF_USESHOWWINDOW | STARTF_FORCEOFFFEEDBACK | STARTF_USESTDHANDLES;
    startupInfo.wShowWindow = SW_HIDE;
    startupInfo.hStdInput = inputReadHandle;
    startupInfo.hStdOutput = outputWriteHandle;
    startupInfo.hStdError = outputWriteHandle;

    status = PhCreateProcessWin32Ex(
        NULL,
        PhGetString(lxssCommandLine),
        NULL,
        NULL,
        &startupInfo,
        PH_CREATE_PROCESS_INHERIT_HANDLES | PH_CREATE_PROCESS_NEW_CONSOLE | PH_CREATE_PROCESS_DEFAULT_ERROR_MODE,
        NULL,
        NULL,
        &processHandle,
        NULL
        );

    if (!NT_SUCCESS(status))
    {
        NtClose(outputWriteHandle);
        NtClose(outputReadHandle);
        NtClose(inputReadHandle);
        NtClose(inputWriteHandle);
        return FALSE;
    }

    // Note: Close the write handles or the child process
    // won't exit and ReadFile will block indefinitely. (dmex)
    NtClose(inputReadHandle);
    NtClose(outputWriteHandle);

    // Read the pipe data. (dmex)
    lxssOutputString = PhGetFileText(outputReadHandle, TRUE);

    // Get the exit code after we finish reading the data from the pipe. (dmex)
    if (NT_SUCCESS(status = PhGetProcessBasicInformation(processHandle, &basicInfo)))
    {
        status = basicInfo.ExitStatus;
    }

    // Note: Don't use NTSTATUS now that we have the lxss exit code. (dmex)
    if (status == 0)
    {
        if (Result) *Result = lxssOutputString;
        if (processHandle) NtClose(processHandle);
        if (outputReadHandle) NtClose(outputReadHandle);
        if (inputWriteHandle) NtClose(inputWriteHandle);

        return TRUE;
    }
    else
    {
        if (lxssOutputString) PhDereferenceObject(lxssOutputString);
        if (processHandle) NtClose(processHandle);
        if (outputReadHandle) NtClose(outputReadHandle);
        if (inputWriteHandle) NtClose(inputWriteHandle);

        return FALSE;
    }
}
