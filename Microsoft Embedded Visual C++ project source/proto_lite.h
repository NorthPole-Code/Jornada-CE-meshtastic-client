#pragma once

#include <windows.h>

// A tiny protobuf encoder/decoder sufficient for Meshtastic stream use.
// Supports: varint, length-delimited, fixed32.

namespace ProtoLite
{
    // Encoding
    int WriteVarint(BYTE* out, int outMax, DWORD value);
    int WriteTag(BYTE* out, int outMax, int fieldNumber, int wireType);
    int WriteFixed32(BYTE* out, int outMax, int fieldNumber, DWORD value);
    int WriteBytes(BYTE* out, int outMax, int fieldNumber, const BYTE* data, int len);
    int WriteString(BYTE* out, int outMax, int fieldNumber, const char* str);
    int WriteMessage(BYTE* out, int outMax, int fieldNumber, const BYTE* msg, int msgLen);

    // Decoding helpers
    bool ReadVarint(const BYTE* buf, int len, int& pos, DWORD& out);
    bool ReadFixed32(const BYTE* buf, int len, int& pos, DWORD& out);
    bool ReadTag(const BYTE* buf, int len, int& pos, int& fieldNumber, int& wireType);
    bool SkipField(const BYTE* buf, int len, int& pos, int wireType);
}
