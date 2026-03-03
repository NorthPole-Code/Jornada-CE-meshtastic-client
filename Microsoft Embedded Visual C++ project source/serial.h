#pragma once

#include <windows.h>

//--------------------------------------------------
// Serial port communication
//--------------------------------------------------
class SerialPort
{
public:
    SerialPort();
    ~SerialPort();

    bool Open(int portNum, DWORD baudRate = 115200);
    void Close();
    bool IsOpen() const { return m_hComm != INVALID_HANDLE_VALUE; }

    int Read(BYTE* pBuffer, int maxBytes);
    bool Write(const BYTE* pBuffer, int numBytes);
    bool WriteLine(const char* pszText);

private:
    HANDLE m_hComm;
};

//--------------------------------------------------
// Serial callbacks (raw bytes)
//--------------------------------------------------
typedef void (*SERIAL_BYTES_CALLBACK)(const unsigned char* pData, int len, bool isRx);

void Serial_Init(SERIAL_BYTES_CALLBACK callback);
void Serial_Shutdown();
bool Serial_Open(int portNum);
void Serial_Close();
bool Serial_IsOpen();
bool Serial_SendBytes(const void* pData, int len);
void Serial_ProcessIncoming(); // Call from timer/thread

// For UI activity indicator
DWORD Serial_GetLastRxTick();
DWORD Serial_GetLastTxTick();

extern SerialPort g_Serial;
