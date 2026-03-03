#include "stdafx.h"
#include "proto_lite.h"
#include <string.h>

namespace ProtoLite
{
    int WriteVarint(BYTE* out, int outMax, DWORD value)
    {
        int written = 0;
        while (true)
        {
            if (written >= outMax)
                return 0;

            BYTE byte = (BYTE)(value & 0x7F);
            value >>= 7;
            if (value)
                byte |= 0x80;

            out[written++] = byte;
            if (!value)
                break;
        }
        return written;
    }

    int WriteTag(BYTE* out, int outMax, int fieldNumber, int wireType)
    {
        DWORD tag = ((DWORD)fieldNumber << 3) | (DWORD)(wireType & 0x7);
        return WriteVarint(out, outMax, tag);
    }

    int WriteFixed32(BYTE* out, int outMax, int fieldNumber, DWORD value)
    {
        int pos = 0;
        int n = WriteTag(out + pos, outMax - pos, fieldNumber, 5); // fixed32
        if (!n) return 0;
        pos += n;
        if (outMax - pos < 4) return 0;
        out[pos++] = (BYTE)(value & 0xFF);
        out[pos++] = (BYTE)((value >> 8) & 0xFF);
        out[pos++] = (BYTE)((value >> 16) & 0xFF);
        out[pos++] = (BYTE)((value >> 24) & 0xFF);
        return pos;
    }

    int WriteBytes(BYTE* out, int outMax, int fieldNumber, const BYTE* data, int len)
    {
        if (!data || len < 0)
            return 0;
        int pos = 0;
        int n = WriteTag(out + pos, outMax - pos, fieldNumber, 2); // length-delimited
        if (!n) return 0;
        pos += n;
        n = WriteVarint(out + pos, outMax - pos, (DWORD)len);
        if (!n) return 0;
        pos += n;
        if (outMax - pos < len) return 0;
        memcpy(out + pos, data, len);
        pos += len;
        return pos;
    }

    int WriteString(BYTE* out, int outMax, int fieldNumber, const char* str)
    {
        if (!str)
            return 0;
        int len = (int)strlen(str);
        return WriteBytes(out, outMax, fieldNumber, (const BYTE*)str, len);
    }

    int WriteMessage(BYTE* out, int outMax, int fieldNumber, const BYTE* msg, int msgLen)
    {
        return WriteBytes(out, outMax, fieldNumber, msg, msgLen);
    }

    bool ReadVarint(const BYTE* buf, int len, int& pos, DWORD& out)
    {
        out = 0;
        int shift = 0;
        while (pos < len && shift <= 28)
        {
            BYTE b = buf[pos++];
            out |= (DWORD)(b & 0x7F) << shift;
            if ((b & 0x80) == 0)
                return true;
            shift += 7;
        }
        return false;
    }

    bool ReadFixed32(const BYTE* buf, int len, int& pos, DWORD& out)
    {
        if (pos + 4 > len)
            return false;
        out = (DWORD)buf[pos] |
              ((DWORD)buf[pos + 1] << 8) |
              ((DWORD)buf[pos + 2] << 16) |
              ((DWORD)buf[pos + 3] << 24);
        pos += 4;
        return true;
    }

    bool ReadTag(const BYTE* buf, int len, int& pos, int& fieldNumber, int& wireType)
    {
        DWORD tag;
        if (!ReadVarint(buf, len, pos, tag))
            return false;
        wireType = (int)(tag & 0x7);
        fieldNumber = (int)(tag >> 3);
        return true;
    }

    bool SkipField(const BYTE* buf, int len, int& pos, int wireType)
    {
        switch (wireType)
        {
            case 0: {
                DWORD tmp;
                return ReadVarint(buf, len, pos, tmp);
            }
            case 2: {
                DWORD l;
                if (!ReadVarint(buf, len, pos, l))
                    return false;
                if (pos + (int)l > len)
                    return false;
                pos += (int)l;
                return true;
            }
            case 5: {
                DWORD tmp;
                return ReadFixed32(buf, len, pos, tmp);
            }
            default:
                // Other wire types are not used by our minimal parsing.
                return false;
        }
    }
}
