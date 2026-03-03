#include "stdafx.h"
#include "serial.h"
#include <string.h>

SerialPort g_Serial;
static SERIAL_BYTES_CALLBACK g_SerialCallback = NULL;
static DWORD g_LastRxTick = 0;
static DWORD g_LastTxTick = 0;

//--------------------------------------------------
// SerialPort class implementation
//--------------------------------------------------
SerialPort::SerialPort()
    : m_hComm(INVALID_HANDLE_VALUE)
{
}

SerialPort::~SerialPort()
{
    Close();
}

bool SerialPort::Open(int portNum, DWORD baudRate)
{
    Close();

    TCHAR portName[32];
    _stprintf(portName, TEXT("COM%d:"), portNum);

    m_hComm = CreateFile(portName,
                         GENERIC_READ | GENERIC_WRITE,
                         0,
                         NULL,
                         OPEN_EXISTING,
                         0,
                         NULL);

    if (m_hComm == INVALID_HANDLE_VALUE)
        return false;

    DCB dcb;
    ZeroMemory(&dcb, sizeof(dcb));
    dcb.DCBlength = sizeof(dcb);

    if (!GetCommState(m_hComm, &dcb))
    {
        Close();
        return false;
    }

    dcb.BaudRate = baudRate;
    dcb.ByteSize = 8;
    dcb.Parity   = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary  = TRUE;
    dcb.fParity  = FALSE;

    if (!SetCommState(m_hComm, &dcb))
    {
        Close();
        return false;
    }

    COMMTIMEOUTS ct;
    ZeroMemory(&ct, sizeof(ct));
    ct.ReadIntervalTimeout         = MAXDWORD;
    ct.ReadTotalTimeoutMultiplier  = 0;
    ct.ReadTotalTimeoutConstant    = 0;
    ct.WriteTotalTimeoutMultiplier = 0;
    ct.WriteTotalTimeoutConstant   = 1000;

    SetCommTimeouts(m_hComm, &ct);

    // Purge any old data
    PurgeComm(m_hComm, PURGE_RXCLEAR | PURGE_TXCLEAR);

    return true;
}

void SerialPort::Close()
{
    if (m_hComm != INVALID_HANDLE_VALUE)
    {
        CloseHandle(m_hComm);
        m_hComm = INVALID_HANDLE_VALUE;
    }
}

int SerialPort::Read(BYTE* pBuffer, int maxBytes)
{
    if (m_hComm == INVALID_HANDLE_VALUE)
        return 0;

    DWORD dwRead = 0;
    if (!ReadFile(m_hComm, pBuffer, maxBytes, &dwRead, NULL))
        return 0;

    return (int)dwRead;
}

bool SerialPort::Write(const BYTE* pBuffer, int numBytes)
{
    if (m_hComm == INVALID_HANDLE_VALUE)
        return false;

    DWORD dwWritten = 0;
    return (WriteFile(m_hComm, pBuffer, numBytes, &dwWritten, NULL) &&
            (int)dwWritten == numBytes);
}

bool SerialPort::WriteLine(const char* pszText)
{
    if (m_hComm == INVALID_HANDLE_VALUE)
        return false;

    int len = strlen(pszText);
    char buffer[512];
    
    if (len + 2 > sizeof(buffer))
        return false;

    memcpy(buffer, pszText, len);
    buffer[len] = '\r';
    buffer[len+1] = '\n';

    return Write((const BYTE*)buffer, len + 2);
}

//--------------------------------------------------
// Global serial monitor functions
//--------------------------------------------------
void Serial_Init(SERIAL_BYTES_CALLBACK callback)
{
    g_SerialCallback = callback;
}

void Serial_Shutdown()
{
    Serial_Close();
    g_SerialCallback = NULL;
}

bool Serial_Open(int portNum)
{
    return g_Serial.Open(portNum);
}

void Serial_Close()
{
    g_Serial.Close();
}

bool Serial_IsOpen()
{
    return g_Serial.IsOpen();
}

bool Serial_SendBytes(const void* pData, int len)
{
    if (!g_Serial.IsOpen())
        return false;

    if (!pData || len <= 0)
        return false;

    // Notify callback (TX)
    if (g_SerialCallback)
        g_SerialCallback((const unsigned char*)pData, len, false);

    g_LastTxTick = GetTickCount();

    return g_Serial.Write((const BYTE*)pData, len);
}

void Serial_ProcessIncoming()
{
    if (!g_Serial.IsOpen())
        return;

    BYTE temp[256];
    int nRead = g_Serial.Read(temp, sizeof(temp));
    
    if (nRead <= 0)
        return;

	g_LastRxTick = GetTickCount();
    
    // Debug: Log raw byte count (helpful for debugging)
    static int totalBytes = 0;
    totalBytes += nRead;
    
    // Callback with raw bytes
    if (g_SerialCallback)
        g_SerialCallback(temp, nRead, true);
}

DWORD Serial_GetLastRxTick() { return g_LastRxTick; }
DWORD Serial_GetLastTxTick() { return g_LastTxTick; }
