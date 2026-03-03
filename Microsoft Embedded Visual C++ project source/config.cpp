#include "stdafx.h"
#include "config.h"
#include <stdio.h>

static AppConfig g_AppConfig;
static DeviceSettings g_DeviceSettings;

//--------------------------------------------------
// Simple INI file functions for Windows CE
// Since GetPrivateProfileString is not available on CE
//--------------------------------------------------

static bool ReadIniString(const TCHAR* pszFile, const TCHAR* pszSection, 
                         const TCHAR* pszKey, const TCHAR* pszDefault,
                         TCHAR* pszValue, int maxLen)
{
    // For simplicity, we'll use registry instead of INI files on CE
    // But for now, let's just use defaults
    _tcsncpy(pszValue, pszDefault, maxLen);
    pszValue[maxLen-1] = 0;
    return false;
}

static int ReadIniInt(const TCHAR* pszFile, const TCHAR* pszSection,
                     const TCHAR* pszKey, int defaultVal)
{
    return defaultVal;
}

static bool WriteIniString(const TCHAR* pszFile, const TCHAR* pszSection,
                          const TCHAR* pszKey, const TCHAR* pszValue)
{
    return true;
}

static void GetIniPath(TCHAR* pszPath, int maxLen)
{
    _sntprintf(pszPath, maxLen, TEXT("%s\\config.ini"), g_AppConfig.storagePath);
}

void Config_Init()
{
    // Already initialized with defaults in constructor
}

void Config_LoadApp()
{
    TCHAR iniPath[MAX_PATH];
    GetIniPath(iniPath, MAX_PATH);
    
    // Check if file exists
    HANDLE hFile = CreateFile(iniPath, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
        return; // Use defaults
    
    // Simple text file parser
    char buffer[512];
    DWORD dwRead;
    
    // Read entire file
    if (ReadFile(hFile, buffer, sizeof(buffer)-1, &dwRead, NULL))
    {
        buffer[dwRead] = 0;
        
        // Parse simple key=value format
        char* pLine = buffer;
        char* pEnd = buffer + dwRead;
        
        while (pLine < pEnd)
        {
            // Find end of line
            char* pLineEnd = pLine;
            while (pLineEnd < pEnd && *pLineEnd != '\r' && *pLineEnd != '\n')
                pLineEnd++;
            
            if (pLineEnd > pLine)
            {
                *pLineEnd = 0;
                
                // Parse KEY=VALUE
                char* pEqual = pLine;
                while (pEqual < pLineEnd && *pEqual != '=')
                    pEqual++;
                
                if (*pEqual == '=')
                {
                    *pEqual = 0;
                    char* pKey = pLine;
                    char* pValue = pEqual + 1;
                    
                    // Trim spaces
                    while (*pKey == ' ' || *pKey == '\t') pKey++;
                    while (*pValue == ' ' || *pValue == '\t') pValue++;
                    
                    // Match app settings keys only
                    if (strcmp(pKey, "StoragePath") == 0)
                    {
#ifdef UNICODE
                        MultiByteToWideChar(CP_ACP, 0, pValue, -1, g_AppConfig.storagePath, MAX_PATH);
#else
                        strncpy(g_AppConfig.storagePath, pValue, MAX_PATH-1);
#endif
                    }
                    else if (strcmp(pKey, "ComPort") == 0)
                    {
                        g_AppConfig.comPort = atoi(pValue);
                    }
                    else if (strcmp(pKey, "ChatTimestampMode") == 0)
                    {
                        g_AppConfig.chatTimestampMode = atoi(pValue);
                        if (g_AppConfig.chatTimestampMode != 1)
                            g_AppConfig.chatTimestampMode = 0;
                    }
                    else if (strcmp(pKey, "ChatAutoScroll") == 0)
                    {
                        g_AppConfig.chatAutoScroll = atoi(pValue);
                        // Backwards compatible:
                        // older builds stored 0/1. New builds allow 0/1/2.
                        if (g_AppConfig.chatAutoScroll < 0 || g_AppConfig.chatAutoScroll > 2)
                            g_AppConfig.chatAutoScroll = 1;
                    }
                    else if (strcmp(pKey, "LedNewMessage") == 0)
                    {
                        g_AppConfig.ledNewMessage = atoi(pValue) ? 1 : 0;
                    }
                    else if (strcmp(pKey, "LedUnread") == 0)
                    {
                        g_AppConfig.ledUnread = atoi(pValue) ? 1 : 0;
                    }
                    else if (strcmp(pKey, "PreventSleepWhileConnected") == 0)
                    {
                        g_AppConfig.preventSleepWhileConnected = atoi(pValue) ? 1 : 0;
                    }
                    else if (strcmp(pKey, "MapInvertColors") == 0)
                    {
                        g_AppConfig.mapInvertColors = atoi(pValue) ? 1 : 0;
                    }
                    else if (strcmp(pKey, "SerialMonitorEnabled") == 0)
                    {
                        g_AppConfig.serialMonitorEnabled = atoi(pValue) ? 1 : 0;
                    }
                    else if (strcmp(pKey, "SerialMonitorMaxLines") == 0)
                    {
                        g_AppConfig.serialMonitorMaxLines = atoi(pValue);
                    }
                }
            }
            
            // Move to next line
            pLine = pLineEnd + 1;
            while (pLine < pEnd && (*pLine == '\r' || *pLine == '\n'))
                pLine++;
        }
    }
    
    CloseHandle(hFile);
}

void Config_SaveApp()
{
    // Ensure directory exists first
    if (!Config_EnsureStorageExists())
        return;
    
    TCHAR iniPath[MAX_PATH];
    GetIniPath(iniPath, MAX_PATH);
    
    HANDLE hFile = CreateFile(iniPath, GENERIC_WRITE, 0, NULL, 
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    
    if (hFile == INVALID_HANDLE_VALUE)
        return;
    
    // Write simple KEY=VALUE format for app settings only
    char line[256];
    DWORD dwWritten;
    
    // Storage Path
    sprintf(line, "StoragePath=");
#ifdef UNICODE
    char temp[MAX_PATH];
    WideCharToMultiByte(CP_ACP, 0, g_AppConfig.storagePath, -1, temp, MAX_PATH, NULL, NULL);
    strcat(line, temp);
#else
    strcat(line, g_AppConfig.storagePath);
#endif
    strcat(line, "\r\n");
    WriteFile(hFile, line, strlen(line), &dwWritten, NULL);
    
    // COM Port
    sprintf(line, "ComPort=%d\r\n", g_AppConfig.comPort);
    WriteFile(hFile, line, strlen(line), &dwWritten, NULL);

    // Chat timestamp mode
    sprintf(line, "ChatTimestampMode=%d\r\n", g_AppConfig.chatTimestampMode);
    WriteFile(hFile, line, strlen(line), &dwWritten, NULL);

    // Auto-scroll (follow new messages)
    sprintf(line, "ChatAutoScroll=%d\r\n", g_AppConfig.chatAutoScroll);
    WriteFile(hFile, line, strlen(line), &dwWritten, NULL);

    // Jornada button LED behavior
    sprintf(line, "LedNewMessage=%d\r\n", g_AppConfig.ledNewMessage ? 1 : 0);
    WriteFile(hFile, line, strlen(line), &dwWritten, NULL);
    sprintf(line, "LedUnread=%d\r\n", g_AppConfig.ledUnread ? 1 : 0);
    WriteFile(hFile, line, strlen(line), &dwWritten, NULL);

    // Power management behavior
    sprintf(line, "PreventSleepWhileConnected=%d\r\n", g_AppConfig.preventSleepWhileConnected ? 1 : 0);
    WriteFile(hFile, line, strlen(line), &dwWritten, NULL);

    // Map viewer behavior
    sprintf(line, "MapInvertColors=%d\r\n", g_AppConfig.mapInvertColors ? 1 : 0);
    WriteFile(hFile, line, strlen(line), &dwWritten, NULL);

    // Serial monitor behavior
    sprintf(line, "SerialMonitorEnabled=%d\r\n", g_AppConfig.serialMonitorEnabled ? 1 : 0);
    WriteFile(hFile, line, strlen(line), &dwWritten, NULL);
    sprintf(line, "SerialMonitorMaxLines=%d\r\n", g_AppConfig.serialMonitorMaxLines);
    WriteFile(hFile, line, strlen(line), &dwWritten, NULL);
    
    CloseHandle(hFile);
}

AppConfig* Config_GetApp()
{
    return &g_AppConfig;
}

DeviceSettings* Config_GetDevice()
{
    return &g_DeviceSettings;
}

bool Config_EnsureStorageExists()
{
    // Try to create the directory
    // On Windows CE, CreateDirectory returns FALSE if already exists
    CreateDirectory(g_AppConfig.storagePath, NULL);
    
    // Check if directory now exists
    DWORD attr = GetFileAttributes(g_AppConfig.storagePath);
    return (attr != 0xFFFFFFFF && (attr & FILE_ATTRIBUTE_DIRECTORY));
}
