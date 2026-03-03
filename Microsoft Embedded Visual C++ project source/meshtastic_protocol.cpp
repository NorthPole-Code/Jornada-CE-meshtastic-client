#include "stdafx.h"
#include "meshtastic_protocol.h"
#include "serial.h"
#include "app.h"
#include "nodes.h"
#include "msgstore.h"
#include "config.h"
#include "settings_ui.h"

void AppendSerialLog(const char* text, bool isRx);

static void LogUnknownField(const char* section, int fieldNum, int wireType)
{
    char buf[96];
    sprintf(buf, "DEBUG: Unknown %s field=%d wire=%d", section, fieldNum, wireType);
    AppendSerialLog(buf, false);
}


#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// ------------------------------------------------------------
// Meshtastic "Client API" over serial
// Framing: 0x94 0xC3 + u16be length + protobuf payload
// Payload messages: meshtastic.ToRadio and meshtastic.FromRadio
// Source: Meshtastic docs / protobufs definitions.
// ------------------------------------------------------------

// Constants
static const DWORD BROADCAST_ADDR = 0xFFFFFFFF;
static const unsigned char FRAME_MAGIC1 = 0x94;
static const unsigned char FRAME_MAGIC2 = 0xC3;

// ACK marker layout (fixed width)
#define ACK_MARKER_LEN 4
#define ACK_PENDING_CHAT TEXT("  - ")
#define ACK_MESH_CHAT    TEXT("  + ")
#define ACK_BLANK_CHAT   TEXT("    ")



// Local state
static DWORD g_MyNodeNum = 0;
static DWORD g_WantConfigNonce = 1;
static DWORD g_LastWantConfigNonceSent = 0;

// Fallback for "stuck hourglass": some firmwares/builds don't always deliver (or we might miss)
// the config_complete_id message during heavy RX bursts.
//
// We track whether we saw *any* config chunks while a settings fetch is active. If so, the first
// subsequent heartbeat response is treated as a safe "config fetch has settled" signal.
static bool g_ConfigChunksSeenWhileFetching = false;

// Admin session passkey (required by AdminMessage for set_* operations)
static unsigned char g_AdminPasskey[32];
static int g_AdminPasskeyLen = 0;
static bool g_AdminPasskeyRequested = false;

// Small ring buffer for framing
static unsigned char g_InBuf[8192];
static int g_InLen = 0;

// ------------------------------------------------------------
// Outgoing message ACK tracking (broadcast chat UI)
// We render a fixed-width 3-char marker at the end of the outgoing line:
//   " - " = no ack yet (default)
//   " + " = ack by mesh
//   "++ " = direct ack by DM recipient (DMs handled in nodes.cpp)
// For broadcast chat we only use " - " / " + ".
// Marker positions are stored as TCHAR character offsets in the chat edit control.
// ------------------------------------------------------------
struct ChatAckTrack
{
    DWORD packetId;
    int markerPos; // TCHAR index in edit control
    bool used;
};

static ChatAckTrack g_ChatAcks[64];

// Register an outgoing broadcast-chat message so we can update its marker on ack.
// markerPos must point at the start of the 3-char marker inside the edit control.
void Meshtastic_RegisterChatOutgoing(DWORD packetId, int markerPos)
{
    if (!packetId || markerPos < 0) return;
    // Replace any existing entry with same id
    int i;
    for (i = 0; i < 64; ++i)
    {
        if (g_ChatAcks[i].used && g_ChatAcks[i].packetId == packetId)
        {
            g_ChatAcks[i].markerPos = markerPos;
            return;
        }
    }
    for (i = 0; i < 64; ++i)
    {
        if (!g_ChatAcks[i].used)
        {
            g_ChatAcks[i].used = true;
            g_ChatAcks[i].packetId = packetId;
            g_ChatAcks[i].markerPos = markerPos;
            return;
        }
    }
    // If full, drop oldest (simple: shift)
    for (i = 1; i < 64; ++i)
        g_ChatAcks[i - 1] = g_ChatAcks[i];
    g_ChatAcks[63].used = true;
    g_ChatAcks[63].packetId = packetId;
    g_ChatAcks[63].markerPos = markerPos;
}


static void ChatAck_UpdateMarker(DWORD requestId, bool success)
{
    if (!g_App.hChatHistory) return;
    if (!requestId) return;

    for (int i = 0; i < 64; ++i)
    {
        if (g_ChatAcks[i].used && g_ChatAcks[i].packetId == requestId)
        {
            const TCHAR* marker = success ? TEXT("  + ") : TEXT("  - ");
            SendMessage(g_App.hChatHistory, WM_SETREDRAW, FALSE, 0);
            SendMessage(g_App.hChatHistory, EM_SETSEL, (WPARAM)g_ChatAcks[i].markerPos, (LPARAM)(g_ChatAcks[i].markerPos + ACK_MARKER_LEN));
            SendMessage(g_App.hChatHistory, EM_REPLACESEL, FALSE, (LPARAM)marker);
            SendMessage(g_App.hChatHistory, WM_SETREDRAW, TRUE, 0);
            InvalidateRect(g_App.hChatHistory, NULL, TRUE);
            // Keep entry, but it is no longer pending.
            g_ChatAcks[i].used = false;
            return;
        }
    }
}

// ------------------------------------------------------------
// Tiny protobuf helpers (enough for our usecase)
// ------------------------------------------------------------
enum WireType {
    WT_VARINT = 0,
    WT_64BIT  = 1,
    WT_LEN    = 2,
    WT_32BIT  = 5,
};

static void pb_write_varint(unsigned __int64 v, unsigned char* out, int* ioLen, int maxLen);
static void pb_write_key(int fieldNum, WireType wt, unsigned char* out, int* ioLen, int maxLen);
static void pb_write_fixed32(DWORD v, unsigned char* out, int* ioLen, int maxLen);
static void pb_write_bytes(int fieldNum, const unsigned char* data, int len, unsigned char* out, int* ioLen, int maxLen);
static void pb_write_u32(int fieldNum, DWORD v, unsigned char* out, int* ioLen, int maxLen);
static void pb_write_bool(int fieldNum, bool v, unsigned char* out, int* ioLen, int maxLen);
static bool pb_read_varint(const unsigned char* data, int len, int* ioPos, unsigned __int64* out);
static bool pb_skip_field(WireType wt, const unsigned char* data, int len, int* ioPos);

// TX helper declared later
static bool SendToRadioPayload(const unsigned char* toRadioPayload, int toRadioLen);

// ------------------------------------------------------------
// Packet de-duplication (best-effort)
// Some firmwares/modules may replay older packets after reconnect.
// We keep a small in-memory ring of recently seen MeshPacket.id values.
// ------------------------------------------------------------
static bool g_AckBlinkOn = true;

void Meshtastic_TickAckBlink()
{
    if (!g_App.hChatHistory) return;
    g_AckBlinkOn = !g_AckBlinkOn;

    for (int i = 0; i < 64; ++i)
    {
        if (!g_ChatAcks[i].used) continue;

        // Blink pending markers by toggling dash visibility
        const TCHAR* marker = g_AckBlinkOn ? ACK_PENDING_CHAT : ACK_BLANK_CHAT;
        SendMessage(g_App.hChatHistory, EM_SETSEL, (WPARAM)g_ChatAcks[i].markerPos, (LPARAM)(g_ChatAcks[i].markerPos + ACK_MARKER_LEN));
            SendMessage(g_App.hChatHistory, EM_REPLACESEL, FALSE, (LPARAM)marker);
    }

    // keep caret/selection stable is not critical for a passive blink
}

static DWORD g_SeenPacketIds[128] = {0};
static int   g_SeenPacketPos = 0;

static bool SeenPacketId(DWORD id)
{
    if (!id) return false;
    for (int i = 0; i < (int)(sizeof(g_SeenPacketIds)/sizeof(g_SeenPacketIds[0])); ++i)
        if (g_SeenPacketIds[i] == id) return true;

    g_SeenPacketIds[g_SeenPacketPos++] = id;
    if (g_SeenPacketPos >= (int)(sizeof(g_SeenPacketIds)/sizeof(g_SeenPacketIds[0])))
        g_SeenPacketPos = 0;
    return false;
}

// ------------------------------------------------------------
// Admin (settings write support)
// ------------------------------------------------------------
static void HandleAdminMessage(const unsigned char* payload, int payloadLen)
{
    // AdminMessage contains a session_passkey (field 101) which must be echoed
    // back in subsequent set_* requests.
    int pos = 0;
    while (pos < payloadLen)
    {
        unsigned __int64 key = 0;
        if (!pb_read_varint(payload, payloadLen, &pos, &key)) break;
        int field = (int)(key >> 3);
        WireType wt = (WireType)(key & 0x7);

        if (field == 101 && wt == WT_LEN)
        {
            unsigned __int64 l = 0;
            if (!pb_read_varint(payload, payloadLen, &pos, &l)) break;
            if (pos + (int)l > payloadLen) break;

            int copy = (int)l;
            if (copy > (int)sizeof(g_AdminPasskey)) copy = (int)sizeof(g_AdminPasskey);
            memcpy(g_AdminPasskey, payload + pos, copy);
            g_AdminPasskeyLen = copy;

            char msg[96];
            sprintf(msg, "DEBUG: Admin session_passkey received (%d bytes)", g_AdminPasskeyLen);
            AppendSerialLog(msg, false);

            pos += (int)l;
        }
        else
        {
            if (!pb_skip_field(wt, payload, payloadLen, &pos)) break;
        }
    }
}

static bool SendAdminPayload(const unsigned char* adminPayload, int adminLen)
{
    if (!Serial_IsOpen() || !adminPayload || adminLen <= 0)
        return false;

    // Build Data { portnum=ADMIN_APP(6), payload=<AdminMessage> }
    unsigned char dataPb[700];
    int dn = 0;
    pb_write_u32(1, 6, dataPb, &dn, sizeof(dataPb));
    pb_write_bytes(2, adminPayload, adminLen, dataPb, &dn, sizeof(dataPb));

    // MeshPacket { to=myNode, channel=0, decoded=data, want_ack=true }
    unsigned char packetPb[900];
    int pn = 0;
    DWORD to = (g_MyNodeNum != 0) ? g_MyNodeNum : BROADCAST_ADDR;
    pb_write_key(2, WT_32BIT, packetPb, &pn, sizeof(packetPb));
    pb_write_fixed32(to, packetPb, &pn, sizeof(packetPb));
    pb_write_u32(3, 0, packetPb, &pn, sizeof(packetPb));
    pb_write_bytes(4, dataPb, dn, packetPb, &pn, sizeof(packetPb));
    pb_write_bool(10, true, packetPb, &pn, sizeof(packetPb));

    // ToRadio { packet=1 }
    unsigned char toRadioPb[1024];
    int tn = 0;
    pb_write_bytes(1, packetPb, pn, toRadioPb, &tn, sizeof(toRadioPb));

    return SendToRadioPayload(toRadioPb, tn);
}

static void Admin_RequestOwnerPasskeyIfNeeded()
{
    if (g_AdminPasskeyLen > 0 || g_AdminPasskeyRequested)
        return;

    // AdminMessage.get_owner_request = 3 (bool)
    unsigned char adminPb[32];
    int an = 0;
    pb_write_bool(3, true, adminPb, &an, sizeof(adminPb));

    g_AdminPasskeyRequested = true;
    AppendSerialLog("DEBUG: Requesting admin session_passkey (get_owner_request)", false);
    SendAdminPayload(adminPb, an);
}

static void pb_write_varint(unsigned __int64 v, unsigned char* out, int* ioLen, int maxLen)
{
    while (1)
    {
        if (*ioLen >= maxLen) return;
        unsigned char b = (unsigned char)(v & 0x7F);
        v >>= 7;
        if (v) b |= 0x80;
        out[(*ioLen)++] = b;
        if (!v) break;
    }
}

static void pb_write_key(int fieldNum, WireType wt, unsigned char* out, int* ioLen, int maxLen)
{
    unsigned __int64 key = ((unsigned __int64)fieldNum << 3) | (unsigned __int64)wt;
    pb_write_varint(key, out, ioLen, maxLen);
}

static void pb_write_fixed32(DWORD v, unsigned char* out, int* ioLen, int maxLen)
{
    if (*ioLen + 4 > maxLen) return;
    out[(*ioLen)++] = (unsigned char)(v & 0xFF);
    out[(*ioLen)++] = (unsigned char)((v >> 8) & 0xFF);
    out[(*ioLen)++] = (unsigned char)((v >> 16) & 0xFF);
    out[(*ioLen)++] = (unsigned char)((v >> 24) & 0xFF);
}

static void pb_write_bytes(int fieldNum, const unsigned char* data, int len, unsigned char* out, int* ioLen, int maxLen)
{
    pb_write_key(fieldNum, WT_LEN, out, ioLen, maxLen);
    pb_write_varint((unsigned __int64)len, out, ioLen, maxLen);
    if (*ioLen + len > maxLen) return;
    memcpy(out + *ioLen, data, len);
    *ioLen += len;
}

static void pb_write_u32(int fieldNum, DWORD v, unsigned char* out, int* ioLen, int maxLen)
{
    pb_write_key(fieldNum, WT_VARINT, out, ioLen, maxLen);
    pb_write_varint((unsigned __int64)v, out, ioLen, maxLen);
}

static void pb_write_bool(int fieldNum, bool v, unsigned char* out, int* ioLen, int maxLen)
{
    pb_write_u32(fieldNum, v ? 1 : 0, out, ioLen, maxLen);
}

static bool pb_read_varint(const unsigned char* data, int len, int* ioPos, unsigned __int64* out)
{
    unsigned __int64 v = 0;
    int shift = 0;
    while (*ioPos < len && shift < 64)
    {
        unsigned char b = data[(*ioPos)++];
        v |= ((unsigned __int64)(b & 0x7F) << shift);
        if (!(b & 0x80))
        {
            *out = v;
            return true;
        }
        shift += 7;
    }
    return false;
}

static bool pb_skip_field(WireType wt, const unsigned char* data, int len, int* ioPos)
{
    unsigned __int64 v = 0;
    switch (wt)
    {
    case WT_VARINT:
        return pb_read_varint(data, len, ioPos, &v);
    case WT_32BIT:
        if (*ioPos + 4 > len) return false;
        *ioPos += 4;
        return true;
    case WT_64BIT:
        if (*ioPos + 8 > len) return false;
        *ioPos += 8;
        return true;
    case WT_LEN:
        if (!pb_read_varint(data, len, ioPos, &v)) return false;
        if (*ioPos + (int)v > len) return false;
        *ioPos += (int)v;
        return true;
    default:
        return false;
    }
}

// ZigZag decode for sint32/sint64 fields
static __int64 pb_zigzag64(unsigned __int64 v)
{
    return (__int64)((v >> 1) ^ (unsigned __int64)-( ( __int64)(v & 1) ));
}

// WinCE sometimes lacks CP_UTF8 support. Try UTF-8, then fall back to ACP.
static void Utf8ToTChar(const char* utf8, LPTSTR out, int cchOut)
{
    if (!out || cchOut <= 0) return;
    out[0] = 0;
    if (!utf8) return;

#ifdef UNICODE
    int ok = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out, cchOut);
    if (ok <= 0)
        MultiByteToWideChar(CP_ACP, 0, utf8, -1, out, cchOut);
#else
    strncpy(out, utf8, cchOut - 1);
    out[cchOut - 1] = 0;
#endif
}

static int FloatBitsToSNRx10(DWORD bits)
{
    // Interpret IEEE754 float bits without using <float> heavy machinery
    // This is small enough and works on WinCE.
    float f;
    memcpy(&f, &bits, sizeof(float));
    int v = (int)(f * 10.0f);
    return v;
}

static int FloatBitsToMilliVolts(DWORD bits)
{
    float f;
    memcpy(&f, &bits, sizeof(float));
    if (f <= 0.0f) return 0;
    // Voltage is in volts. Convert to millivolts.
    int mv = (int)(f * 1000.0f + 0.5f);
    if (mv < 0) mv = 0;
    return mv;
}

static bool EpochSecondsToSystemTime(DWORD epochSeconds, SYSTEMTIME* outSt)
{
    if (!outSt) return false;
    if (epochSeconds == 0) return false;

    // Convert UNIX epoch seconds to FILETIME (100ns since 1601-01-01)
    // FILETIME for 1970-01-01 is 11644473600 seconds.
    // NOTE: eVC++ (old MSVC) is picky about ULL/LL suffixes, so use __int64 casts.
    ULARGE_INTEGER ui;
    ui.QuadPart = ((unsigned __int64)11644473600 + (unsigned __int64)epochSeconds) * (unsigned __int64)10000000;

    FILETIME ft;
    ft.dwLowDateTime = ui.LowPart;
    ft.dwHighDateTime = ui.HighPart;

    return FileTimeToSystemTime(&ft, outSt) ? true : false;
}

static const TCHAR* HwModelToText(unsigned __int64 m)
{
    // HardwareModel enum is defined in meshtastic/mesh.proto.
    // Keep this mapping in sync with the upstream enum numbers.
    // Not exhaustive - we only include common boards.
    switch ((int)m)
    {
    case 0:  return TEXT("UNSET");

    // LilyGO / TTGO
    case 1:  return TEXT("TLORA_V2");
    case 2:  return TEXT("TLORA_V1");
    case 3:  return TEXT("TLORA_V2_1_1P6");
    case 8:  return TEXT("TLORA_V1_1P3");
    case 12: return TEXT("TBEAM_S3");
    case 15: return TEXT("TLORA_V2_1_1P8");
    case 16: return TEXT("TLORA_T3_S3");
    case 50: return TEXT("T_DECK");
    case 51: return TEXT("T_WATCH_S3");
    case 7:  return TEXT("T_ECHO");

    // Heltec
    case 5:  return TEXT("HELTEC_V2_0");
    case 10: return TEXT("HELTEC_V2_1");
    case 11: return TEXT("HELTEC_V1");
    case 23: return TEXT("HELTEC_HRU_3601");
    case 24: return TEXT("HELTEC_WIRELESS_BRIDGE");
    case 43: return TEXT("HELTEC_V3");
    case 44: return TEXT("HELTEC_WSL_V3");
    case 48: return TEXT("HELTEC_WIRELESS_TRACKER");
    case 49: return TEXT("HELTEC_WIRELESS_PAPER");
    case 57: return TEXT("HELTEC_WIRELESS_PAPER_V1_0");
    case 58: return TEXT("HELTEC_WIRELESS_TRACKER_V1_0");
    case 65: return TEXT("HELTEC_CAPSULE_SENSOR_V3");
    case 66: return TEXT("HELTEC_VISION_MASTER_T190");
    case 67: return TEXT("HELTEC_VISION_MASTER_E213");
    case 68: return TEXT("HELTEC_VISION_MASTER_E290");
    case 69: return TEXT("HELTEC_MESH_NODE_T114");

    // RAK / WisBlock
    case 9:  return TEXT("RAK4631");
    case 13: return TEXT("RAK11200");
    case 26: return TEXT("RAK11310");
    case 22: return TEXT("RAK2560");

    // Other popular boards
    case 4:  return TEXT("TBEAM");
    case 42: return TEXT("M5STACK");
    case 47: return TEXT("RPI_PICO");
    case 70: return TEXT("SENSECAP_INDICATOR");
    case 71: return TEXT("TRACKER_T1000_E");

    default: return NULL;
    }
}

// ------------------------------------------------------------
// Parsing of specific messages we care about
// ------------------------------------------------------------

static void LogFrame(const char* prefix, const unsigned char* payload, int payloadLen)
{
    // Skip serial monitor formatting entirely when the monitor is disabled.
    AppConfig* cfg = Config_GetApp();
    if (cfg && !cfg->serialMonitorEnabled)
        return;

    // Keep serial monitor useful but lightweight.
    // Show prefix + first 32 bytes as hex.
    char line[256];
    int n = 0;
    n += _snprintf(line + n, sizeof(line) - n, "%s (%d): ", prefix, payloadLen);
    int show = payloadLen;
    if (show > 32) show = 32;
    for (int i = 0; i < show && n < (int)sizeof(line) - 4; i++)
        n += _snprintf(line + n, sizeof(line) - n, "%02X ", payload[i]);
    if (payloadLen > show)
        _snprintf(line + n, sizeof(line) - n, "...");
    AppendSerialLog(line, false);
}

static void AddChatLine(HWND hHistory, DWORD fromNode, const char* text)
{
    if (!hHistory) return;

    // Resolve display name
    TCHAR label[64];
    NodeInfo* pNode = Nodes_FindOrAdd(fromNode);
    if (pNode && (pNode->shortName[0] || pNode->longName[0]))
        _sntprintf(label, 64, TEXT("[%s]"), pNode->shortName[0] ? pNode->shortName : (pNode->longName[0] ? pNode->longName : TEXT("node")));
    else
        _sntprintf(label, 64, TEXT("[0x%08lX]"), fromNode);

    TCHAR tText[512];
    Utf8ToTChar(text, tText, 512);
    MsgStore_AppendChat(hHistory, label, tText);
}

static void HandleNodeInfo(const unsigned char* msg, int msgLen)
{
    // NodeInfo fields we care about:
    // 1: num (varint)
    // 2: user (len)
    // 3: position (len)
    // 5: last_heard (fixed32)
    DWORD num = 0;
    DWORD lastHeard = 0;
    bool hasPos = false;
    int lat_i = 0;
    int lon_i = 0;
    char longName[64] = {0};
    char shortName[16] = {0};
    unsigned __int64 hwModel = (unsigned __int64)-1;

    int pos = 0;
    while (pos < msgLen)
    {
        unsigned __int64 key = 0;
        if (!pb_read_varint(msg, msgLen, &pos, &key)) break;
        int field = (int)(key >> 3);
        WireType wt = (WireType)(key & 0x7);

        if (field == 1 && wt == WT_VARINT)
        {
            unsigned __int64 v = 0;
            if (!pb_read_varint(msg, msgLen, &pos, &v)) break;
            num = (DWORD)v;
        }
        else if (field == 5 && wt == WT_32BIT)
        {
            if (pos + 4 > msgLen) break;
            lastHeard = (DWORD)msg[pos] | ((DWORD)msg[pos+1] << 8) | ((DWORD)msg[pos+2] << 16) | ((DWORD)msg[pos+3] << 24);
            pos += 4;
        }
        else if (field == 2 && wt == WT_LEN)
        {
            unsigned __int64 l = 0;
            if (!pb_read_varint(msg, msgLen, &pos, &l)) break;
            if (pos + (int)l > msgLen) break;

            // Parse User submessage: id=1 (string), long_name=2, short_name=3, hw_model=5 (enum)
            int up = 0;
            const unsigned char* u = msg + pos;
            int ulen = (int)l;
            while (up < ulen)
            {
                unsigned __int64 uk = 0;
                if (!pb_read_varint(u, ulen, &up, &uk)) break;
                int uf = (int)(uk >> 3);
                WireType uw = (WireType)(uk & 0x7);
                if ((uf == 2 || uf == 3) && uw == WT_LEN)
                {
                    unsigned __int64 sl = 0;
                    if (!pb_read_varint(u, ulen, &up, &sl)) break;
                    if (up + (int)sl > ulen) break;
                    if (uf == 2)
                    {
                        int copy = (int)sl;
                        if (copy > (int)sizeof(longName) - 1) copy = sizeof(longName) - 1;
                        memcpy(longName, u + up, copy);
                        longName[copy] = 0;
                    }
                    else
                    {
                        int copy = (int)sl;
                        if (copy > (int)sizeof(shortName) - 1) copy = sizeof(shortName) - 1;
                        memcpy(shortName, u + up, copy);
                        shortName[copy] = 0;
                    }
                    up += (int)sl;
                }
                else if (uf == 5 && uw == WT_VARINT)
                {
                    unsigned __int64 mv = 0;
                    if (!pb_read_varint(u, ulen, &up, &mv)) break;
                    hwModel = mv;
                }
                else
                {
                    if (!pb_skip_field(uw, u, ulen, &up)) break;
                }
            }
            pos += (int)l;
        }
        else if (field == 3 && wt == WT_LEN)
        {
            unsigned __int64 l = 0;
            if (!pb_read_varint(msg, msgLen, &pos, &l)) break;
            if (pos + (int)l > msgLen) break;

            // Parse Position: latitude_i=1 (sfixed32), longitude_i=2 (sfixed32)
            int pp = 0;
            const unsigned char* p = msg + pos;
            int plen = (int)l;
            while (pp < plen)
            {
                unsigned __int64 pk = 0;
                if (!pb_read_varint(p, plen, &pp, &pk)) break;
                int pf = (int)(pk >> 3);
                WireType pw = (WireType)(pk & 0x7);
                if ((pf == 1 || pf == 2) && pw == WT_32BIT)
                {
                    if (pp + 4 > plen) break;
                    int v = (int)((DWORD)p[pp] | ((DWORD)p[pp+1] << 8) | ((DWORD)p[pp+2] << 16) | ((DWORD)p[pp+3] << 24));
                    pp += 4;
                    if (pf == 1) lat_i = v;
                    else lon_i = v;
                    hasPos = true;
                }
                else
                {
                    if (!pb_skip_field(pw, p, plen, &pp)) break;
                }
            }

            pos += (int)l;
        }
        else
        {
            if (!pb_skip_field(wt, msg, msgLen, &pos)) break;
        }
    }

    if (!num) return;

    NodeInfo* n = Nodes_FindOrAdd(num);
    if (!n) return;

        // Update short/long names (Meshtastic user.short_name / user.long_name)
    if (shortName[0])
        Utf8ToTChar(shortName, n->shortName, 16);

    if (longName[0])
        Utf8ToTChar(longName, n->longName, 64);

    // Hardware model (best-effort)
    if (hwModel != (unsigned __int64)-1)
    {
        const TCHAR* mtxt = HwModelToText(hwModel);
        if (mtxt)
            _tcsncpy(n->model, mtxt, 23);
        else
            _sntprintf(n->model, 24, TEXT("%u"), (unsigned int)hwModel);
        n->model[23] = 0;
    }

    // Node ID text (Meshtastic usually shows !XXXXXXXX)
    if (!n->nodeId[0])
        _sntprintf(n->nodeId, 12, TEXT("!%08lX"), n->nodeNum);

    // Prefer the firmware's last_heard (epoch seconds) if provided.
    // Do NOT force "online" here; online/offline is computed based on recency.
    if (lastHeard)
    {
        SYSTEMTIME st;
        if (EpochSecondsToSystemTime(lastHeard, &st))
            n->lastHeard = st;
    }


    if (hasPos)
    {
        n->hasPosition = true;
        n->latitude = ((double)lat_i) * 1e-7;
        n->longitude = ((double)lon_i) * 1e-7;
    }

    Nodes_RebuildList();

    if (g_App.hMapView)
        InvalidateRect(g_App.hMapView, NULL, FALSE);
}

static void HandleMyInfo(const unsigned char* msg, int msgLen)
{
    // MyNodeInfo: my_node_num = 1 (varint)
    int pos = 0;
    while (pos < msgLen)
    {
        unsigned __int64 key = 0;
        if (!pb_read_varint(msg, msgLen, &pos, &key)) break;
        int field = (int)(key >> 3);
        WireType wt = (WireType)(key & 0x7);
        if (field == 1 && wt == WT_VARINT)
        {
            unsigned __int64 v = 0;
            if (!pb_read_varint(msg, msgLen, &pos, &v)) break;
            g_MyNodeNum = (DWORD)v;
            char line[80];
            _snprintf(line, sizeof(line), "MyNodeInfo: my_node_num=0x%08lX", g_MyNodeNum);
            AppendSerialLog(line, false);

            // As soon as we know our node number, request an admin session key.
            // This key is required for set_config/commit operations.
            Admin_RequestOwnerPasskeyIfNeeded();
            return;
        }
        if (!pb_skip_field(wt, msg, msgLen, &pos)) break;
    }
}

static void HandleMeshPacket(const unsigned char* msg, int msgLen)
{
    // MeshPacket fields we care about:
    // from=1 (fixed32), to=2 (fixed32), channel=3 (varint), decoded=4 (len)
    // encrypted=5 (len), id=6 (fixed32), rx_snr=8 (fixed32 float) [best-effort]
    // hop_limit=9 (varint), hop_start=15 (varint)
    DWORD from = 0;
    DWORD to = BROADCAST_ADDR;
    DWORD channel = 0;


    DWORD packetId = 0;
    DWORD hopLimit = 0;
    DWORD hopStart = 0;
    bool  hasHopLimit = false;
    bool  hasHopStart = false;
    bool  hasEncrypted = false;
    DWORD snrBits = 0;
    bool  hasSnr = false;

    const unsigned char* decoded = NULL;
    int decodedLen = 0;

    int pos = 0;
    while (pos < msgLen)
    {
        unsigned __int64 key = 0;
        if (!pb_read_varint(msg, msgLen, &pos, &key)) break;
        int field = (int)(key >> 3);
        WireType wt = (WireType)(key & 0x7);

        if ((field == 1 || field == 2) && wt == WT_32BIT)
        {
            if (pos + 4 > msgLen) break;
            DWORD v = (DWORD)msg[pos] | ((DWORD)msg[pos+1] << 8) | ((DWORD)msg[pos+2] << 16) | ((DWORD)msg[pos+3] << 24);
            pos += 4;
            if (field == 1) from = v;
            else to = v;
        }

        else if (field == 6 && wt == WT_32BIT)
        {
            if (pos + 4 > msgLen) break;
            DWORD v = (DWORD)msg[pos] | ((DWORD)msg[pos+1] << 8) | ((DWORD)msg[pos+2] << 16) | ((DWORD)msg[pos+3] << 24);
            pos += 4;
            packetId = v;
        }
        else if (field == 3 && wt == WT_VARINT)
        {
            unsigned __int64 v = 0;
            if (!pb_read_varint(msg, msgLen, &pos, &v)) break;
            channel = (DWORD)v;
        }
        else if (field == 4 && wt == WT_LEN)
        {
            unsigned __int64 l = 0;
            if (!pb_read_varint(msg, msgLen, &pos, &l)) break;
            if (pos + (int)l > msgLen) break;
            decoded = msg + pos;
            decodedLen = (int)l;
            pos += (int)l;
        }
        else if (field == 5 && wt == WT_LEN)
        {
            // Encrypted payload (we cannot decode it here, but we can mark node as encrypted)
            unsigned __int64 l = 0;
            if (!pb_read_varint(msg, msgLen, &pos, &l)) break;
            if (pos + (int)l > msgLen) break;
            hasEncrypted = true;
            pos += (int)l;
        }
        else if (field == 8 && wt == WT_32BIT)
        {
            if (pos + 4 > msgLen) break;
            snrBits = (DWORD)msg[pos] | ((DWORD)msg[pos+1] << 8) | ((DWORD)msg[pos+2] << 16) | ((DWORD)msg[pos+3] << 24);
            pos += 4;
            hasSnr = true;
        }
        else if (field == 9 && wt == WT_VARINT)
        {
            unsigned __int64 v = 0;
            if (!pb_read_varint(msg, msgLen, &pos, &v)) break;
            hopLimit = (DWORD)v;
            hasHopLimit = true;
        }
        else if (field == 15 && wt == WT_VARINT)
        {
            unsigned __int64 v = 0;
            if (!pb_read_varint(msg, msgLen, &pos, &v)) break;
            hopStart = (DWORD)v;
            hasHopStart = true;
        }
        else
        {
            if (!pb_skip_field(wt, msg, msgLen, &pos)) break;
        }
    }

    // Update per-node radio metrics even if the payload is encrypted
    if (from)
    {
        NodeInfo* n = Nodes_FindOrAdd(from);
        if (n)
        {
            // Mark the node as heard "now".
            GetLocalTime(&n->lastHeard);

            // Over-the-air Meshtastic traffic is normally encrypted when using a PSK.
            // The MeshPacket only includes the 'encrypted' field when the host cannot
            // decrypt it, so using that as the only signal makes the UI misleading.
            // For the client UI, treat any received mesh packet as encrypted traffic.
            n->encrypted = true;
            if (hasHopLimit && hasHopStart && hopStart >= hopLimit)
                n->hopsAway = (int)(hopStart - hopLimit);
            if (hasSnr)
                n->snr_x10 = FloatBitsToSNRx10(snrBits);
        }
    }

    if (!decoded || decodedLen <= 0)
    {
        // Encrypted packets won't have decoded. Still refresh list/map.
        Nodes_RebuildList();
        if (g_App.hMapView) InvalidateRect(g_App.hMapView, NULL, FALSE);
        return;
    }

    // Data: portnum=1 (varint), payload=2 (bytes)
    DWORD portnum = 0;
    DWORD requestId = 0;
    DWORD replyId = 0;
    const unsigned char* payload = NULL;
    int payloadLen = 0;
    int dp = 0;
    while (dp < decodedLen)
    {
        unsigned __int64 key = 0;
        if (!pb_read_varint(decoded, decodedLen, &dp, &key)) break;
        int field = (int)(key >> 3);
        WireType wt = (WireType)(key & 0x7);
        if (field == 1 && wt == WT_VARINT)
        {
            unsigned __int64 v = 0;
            if (!pb_read_varint(decoded, decodedLen, &dp, &v)) break;
            portnum = (DWORD)v;
        }
        else if (field == 2 && wt == WT_LEN)
        {
            unsigned __int64 l = 0;
            if (!pb_read_varint(decoded, decodedLen, &dp, &l)) break;
            if (dp + (int)l > decodedLen) break;
            payload = decoded + dp;
            payloadLen = (int)l;
            dp += (int)l;
        }
        else if ((field == 6 || field == 7) && wt == WT_32BIT)
        {
            if (dp + 4 > decodedLen) break;
            DWORD v = (DWORD)decoded[dp] | ((DWORD)decoded[dp+1] << 8) | ((DWORD)decoded[dp+2] << 16) | ((DWORD)decoded[dp+3] << 24);
            dp += 4;
            if (field == 6) requestId = v; else replyId = v;
        }
        else
        {
            if (!pb_skip_field(wt, decoded, decodedLen, &dp)) break;
        }
    }

    // ROUTING_APP (5): ack/nak for a previously sent message.
    // Typically indicates the mesh/network ACK; for DMs the recipient might also directly ACK.
    if (portnum == 5 && requestId)
    {
        bool success = true;
        // If payload contains Routing.error_reason (field 3), treat non-zero as failure.
        if (payload && payloadLen > 0)
        {
            int rp = 0;
            while (rp < payloadLen)
            {
                unsigned __int64 rk = 0;
                if (!pb_read_varint(payload, payloadLen, &rp, &rk)) break;
                int rf = (int)(rk >> 3);
                WireType rw = (WireType)(rk & 0x7);
                if (rf == 3 && rw == WT_VARINT)
                {
                    unsigned __int64 ev = 0;
                    if (!pb_read_varint(payload, payloadLen, &rp, &ev)) break;
                    if (ev != 0) success = false;
                }
                else
                {
                    if (!pb_skip_field(rw, payload, payloadLen, &rp)) break;
                }
            }
        }

        // Update UI markers
        ChatAck_UpdateMarker(requestId, success);
        Direct_OnRoutingAck(from, requestId, success);
        return;
    }

    // ADMIN_APP (6): capture session_passkey for subsequent set_config operations.
    if (portnum == 6 && payload && payloadLen > 0)
    {
        HandleAdminMessage(payload, payloadLen);
        return;
    }

    // POSITION_APP (3): update map position continuously
    if (portnum == 3 && payload && payloadLen > 0 && from)
    {
        int lat_i = 0, lon_i = 0;
        bool hasPos = false;
        int pp = 0;
        while (pp < payloadLen)
        {
            unsigned __int64 pk = 0;
            if (!pb_read_varint(payload, payloadLen, &pp, &pk)) break;
            int pf = (int)(pk >> 3);
            WireType pw = (WireType)(pk & 0x7);
            if ((pf == 1 || pf == 2) && pw == WT_32BIT)
            {
                if (pp + 4 > payloadLen) break;
                int v = (int)((DWORD)payload[pp] | ((DWORD)payload[pp+1] << 8) | ((DWORD)payload[pp+2] << 16) | ((DWORD)payload[pp+3] << 24));
                pp += 4;
                if (pf == 1) lat_i = v; else lon_i = v;
                hasPos = true;
            }
            else
            {
                if (!pb_skip_field(pw, payload, payloadLen, &pp)) break;
            }
        }

        if (hasPos)
        {
            NodeInfo* n = Nodes_FindOrAdd(from);
            if (n)
            {
                n->hasPosition = true;
                n->latitude = ((double)lat_i) * 1e-7;
                n->longitude = ((double)lon_i) * 1e-7;
                Nodes_RebuildList();
                if (g_App.hMapView) InvalidateRect(g_App.hMapView, NULL, FALSE);
            }
        }
        return;
    }

    // TELEMETRY_APP (67): best-effort DeviceMetrics parsing for battery
    // Expected shape: Telemetry { device_metrics=1 (len) } and DeviceMetrics { battery_level=1 (varint) }
    if (portnum == 67 && payload && payloadLen > 0 && from)
    {
        int tp = 0;
        while (tp < payloadLen)
        {
            unsigned __int64 tk = 0;
            if (!pb_read_varint(payload, payloadLen, &tp, &tk)) break;
            int tf = (int)(tk >> 3);
            WireType tw = (WireType)(tk & 0x7);
            if (tf == 1 && tw == WT_LEN)
            {
                unsigned __int64 dl = 0;
                if (!pb_read_varint(payload, payloadLen, &tp, &dl)) break;
                if (tp + (int)dl > payloadLen) break;

                const unsigned char* d = payload + tp;
                int dlen = (int)dl;
                int dp2 = 0;
                int batt = 0;
                bool hasBatt = false;
                DWORD voltBits = 0;
                bool hasVolt = false;
                while (dp2 < dlen)
                {
                    unsigned __int64 dk = 0;
                    if (!pb_read_varint(d, dlen, &dp2, &dk)) break;
                    int df = (int)(dk >> 3);
                    WireType dw = (WireType)(dk & 0x7);
                    if (df == 1 && dw == WT_VARINT)
                    {
                        unsigned __int64 bv = 0;
                        if (!pb_read_varint(d, dlen, &dp2, &bv)) break;
                        batt = (int)bv;
                        hasBatt = true;
                    }
                    else if (df == 2 && dw == WT_32BIT)
                    {
                        if (dp2 + 4 > dlen) break;
                        voltBits = (DWORD)d[dp2] | ((DWORD)d[dp2+1] << 8) | ((DWORD)d[dp2+2] << 16) | ((DWORD)d[dp2+3] << 24);
                        dp2 += 4;
                        hasVolt = true;
                    }
                    else
                    {
                        if (!pb_skip_field(dw, d, dlen, &dp2)) break;
                    }
                }

                if (hasBatt || hasVolt)
                {
                    NodeInfo* n = Nodes_FindOrAdd(from);
                    if (n)
                    {
                        if (hasBatt)
                        {
                            if (batt < 0) batt = 0; if (batt > 100) batt = 100;
                            n->batteryPct = batt;
                        }
                        if (hasVolt)
                        {
                            int mv = FloatBitsToMilliVolts(voltBits);
                            if (mv > 0) n->batteryMv = mv;
                        }
                        GetLocalTime(&n->lastHeard);
                        Nodes_RebuildList();
                    }
                }

                tp += (int)dl;
            }
            else
            {
                if (!pb_skip_field(tw, payload, payloadLen, &tp)) break;
            }
        }
        return;
    }

    // TEXT_MESSAGE_APP = 1
    if (portnum == 1 && payload && payloadLen > 0)
    {
        
        // Best-effort de-duplication using MeshPacket.id
        if (packetId && SeenPacketId(packetId))
            return;

// Ensure null-terminated UTF-8
        char text[512];
        int copy = payloadLen;
        if (copy > (int)sizeof(text) - 1) copy = sizeof(text) - 1;
        memcpy(text, payload, copy);
        text[copy] = 0;

        // Update last-heard timestamp for the sender
        if (from)
        {
            NodeInfo* n = Nodes_FindOrAdd(from);
            if (n)
            {
                GetLocalTime(&n->lastHeard);
                n->encrypted = true;
                Nodes_RebuildList();
            }
        }

        if (to == BROADCAST_ADDR)
        {
            AddChatLine(g_App.hChatHistory, from, text);
            UI_Unread_OnIncomingChat();
        }
        else
        {
            // DM - route to per-node conversations list
            Direct_OnIncomingText(from, text);
            UI_Unread_OnIncomingDM(from);
        }
    }

    // POSITION_APP = 3 (update map)
    if (portnum == 3 && payload && payloadLen > 0 && from)
    {
        // Position: latitude_i=1 (sfixed32), longitude_i=2 (sfixed32)
        bool hasPos = false;
        int lat_i = 0, lon_i = 0;
        int pp = 0;
        while (pp < payloadLen)
        {
            unsigned __int64 pk = 0;
            if (!pb_read_varint(payload, payloadLen, &pp, &pk)) break;
            int pf = (int)(pk >> 3);
            WireType pw = (WireType)(pk & 0x7);
            if ((pf == 1 || pf == 2) && pw == WT_32BIT)
            {
                if (pp + 4 > payloadLen) break;
                int v = (int)((DWORD)payload[pp] | ((DWORD)payload[pp+1] << 8) | ((DWORD)payload[pp+2] << 16) | ((DWORD)payload[pp+3] << 24));
                pp += 4;
                if (pf == 1) lat_i = v;
                else lon_i = v;
                hasPos = true;
            }
            else
            {
                if (!pb_skip_field(pw, payload, payloadLen, &pp)) break;
            }
        }
        if (hasPos)
        {
            NodeInfo* n = Nodes_FindOrAdd(from);
            if (n)
            {
                n->hasPosition = true;
                n->latitude = ((double)lat_i) * 1e-7;
                n->longitude = ((double)lon_i) * 1e-7;
                Nodes_RebuildList();
                if (g_App.hMapView) InvalidateRect(g_App.hMapView, NULL, FALSE);
            }
        }
    }

    // TELEMETRY_APP = 67 (battery, etc.) - best effort
    if (portnum == 67 && payload && payloadLen > 0 && from)
    {
        // Telemetry { device_metrics=1 (len) ... }
        int tp = 0;
        while (tp < payloadLen)
        {
            unsigned __int64 tk = 0;
            if (!pb_read_varint(payload, payloadLen, &tp, &tk)) break;
            int tf = (int)(tk >> 3);
            WireType tw = (WireType)(tk & 0x7);
            if (tf == 1 && tw == WT_LEN)
            {
                unsigned __int64 l = 0;
                if (!pb_read_varint(payload, payloadLen, &tp, &l)) break;
                if (tp + (int)l > payloadLen) break;
                const unsigned char* dm = payload + tp;
                int dmlen = (int)l;
                int dp2 = 0;
                DWORD batt = 0;
                bool hasBatt = false;
                while (dp2 < dmlen)
                {
                    unsigned __int64 dk = 0;
                    if (!pb_read_varint(dm, dmlen, &dp2, &dk)) break;
                    int df = (int)(dk >> 3);
                    WireType dw = (WireType)(dk & 0x7);
                    if (df == 1 && dw == WT_VARINT)
                    {
                        unsigned __int64 v = 0;
                        if (!pb_read_varint(dm, dmlen, &dp2, &v)) break;
                        batt = (DWORD)v;
                        hasBatt = true;
                    }
                    else
                    {
                        if (!pb_skip_field(dw, dm, dmlen, &dp2)) break;
                    }
                }

                if (hasBatt)
                {
                    NodeInfo* n = Nodes_FindOrAdd(from);
                    if (n)
                    {
                        n->batteryPct = (int)batt;
                        Nodes_RebuildList();
                    }
                }

                tp += (int)l;
            }
            else
            {
                if (!pb_skip_field(tw, payload, payloadLen, &tp)) break;
            }
        }
    }
}

static void HandleConfig(const unsigned char* payload, int payloadLen)
{
    // Config message fields:
    // device=1, position=2, power=3, network=4, display=5, lora=6, bluetooth=7

    // If the Settings tab triggered a fetch, remember that we did in fact receive config chunks.
    // We'll use this in HandleFromRadio() as a fallback completion signal if config_complete_id
    // never arrives.
    if (g_App.isFetchingSettings)
        g_ConfigChunksSeenWhileFetching = true;
    
    int pos = 0;
    while (pos < payloadLen)
    {
        unsigned __int64 key = 0;
        if (!pb_read_varint(payload, payloadLen, &pos, &key)) break;
        int field = (int)(key >> 3);
        WireType wt = (WireType)(key & 0x7);

        if (field == 1 && wt == WT_LEN)  // device config
        {
            unsigned __int64 l = 0;
            if (!pb_read_varint(payload, payloadLen, &pos, &l)) break;
            if (pos + (int)l > payloadLen) break;

            const unsigned char* devSub = payload + pos;
            int devLen = (int)l;
            
            // Parse Device config
            DeviceSettings* devCfg = Config_GetDevice();
            int devPos = 0;
            
            AppendSerialLog("DEBUG: Parsing Device config...", false);
            
            while (devPos < devLen)
            {
                unsigned __int64 devKey = 0;
                if (!pb_read_varint(devSub, devLen, &devPos, &devKey)) break;
                int devField = (int)(devKey >> 3);
                WireType devWt = (WireType)(devKey & 0x7);

                if (devField == 1 && devWt == WT_VARINT)  // role
                {
                    unsigned __int64 v = 0;
                    if (pb_read_varint(devSub, devLen, &devPos, &v))
                    {
                        devCfg->role = (int)v;
                        char msg[64];
                        sprintf(msg, "DEBUG: Got role=%d", (int)v);
                        AppendSerialLog(msg, false);
                    }
                }
                else
                {
                    LogUnknownField("DEVICE", devField, (int)devWt);
                    if (!pb_skip_field(devWt, devSub, devLen, &devPos)) break;
                }
            }

            pos += (int)l;
        }
        else if (field == 2 && wt == WT_LEN)  // position config
        {
            unsigned __int64 l = 0;
            if (!pb_read_varint(payload, payloadLen, &pos, &l)) break;
            if (pos + (int)l > payloadLen) break;

            const unsigned char* posSub = payload + pos;
            int posLen = (int)l;
            
            // Parse Position config
            DeviceSettings* devCfg = Config_GetDevice();
            int posPos = 0;
            
            AppendSerialLog("DEBUG: Parsing Position config...", false);
            // gps_mode (field 13): Meshtastic 2.7.x omits field when default (DISABLED=0)
            devCfg->gpsMode = GPS_MODE_DISABLED;
            bool gpsModePresent = false;
            
            while (posPos < posLen)
            {
                unsigned __int64 posKey = 0;
                if (!pb_read_varint(posSub, posLen, &posPos, &posKey)) break;
                int posField = (int)(posKey >> 3);
                WireType posWt = (WireType)(posKey & 0x7);

                if (posField == 1 && posWt == WT_VARINT)  // position_broadcast_secs
                {
                    unsigned __int64 v = 0;
                    if (pb_read_varint(posSub, posLen, &posPos, &v))
                    {
                        devCfg->transmitLocation = (v > 0);
                        char msg[64];
                        sprintf(msg, "DEBUG: Got position_broadcast_secs=%u (transmit=%d)", 
                                (unsigned)v, devCfg->transmitLocation ? 1 : 0);
                        AppendSerialLog(msg, false);
                    }
                }
                else if (posField == 13 && posWt == WT_VARINT)  // gps_mode (default omitted)
                {
                    unsigned __int64 v = 0;
                    if (pb_read_varint(posSub, posLen, &posPos, &v))
                    {
                        devCfg->gpsMode = (int)v;
                        gpsModePresent = true;
                        char msg[64];
                        sprintf(msg, "DEBUG: Got gps_mode=%d", (int)v);
                        AppendSerialLog(msg, false);
                    }
                }
                else
                {
                    LogUnknownField("POSITION", posField, (int)posWt);
                    if (!pb_skip_field(posWt, posSub, posLen, &posPos)) break;
                }
            }

            if (!gpsModePresent)
            {
                AppendSerialLog("DEBUG: gps_mode not present -> default DISABLED(0)", false);
            }

            pos += (int)l;
        }
        else if (field == 6 && wt == WT_LEN)  // lora config
        {
            unsigned __int64 l = 0;
            if (!pb_read_varint(payload, payloadLen, &pos, &l)) break;
            if (pos + (int)l > payloadLen) break;

            const unsigned char* loraSub = payload + pos;
            int loraLen = (int)l;
            
            // Parse LoRa config
            DeviceSettings* devCfg = Config_GetDevice();
            int loraPos = 0;
            
            AppendSerialLog("DEBUG: Parsing LoRa config...", false);
            // LoRaConfig.tx_enabled is a bool; protobuf omits it when FALSE (default).
            // So if the field is not present on the wire, interpret that as disabled.
            devCfg->txEnabled = false;
            devCfg->txEnabledPresent = false;
            
            
            // modem_preset should be trusted if present; only fall back to BW/SF detection if absent.
            devCfg->modemPresetPresent = false;
while (loraPos < loraLen)
            {
                unsigned __int64 loraKey = 0;
                if (!pb_read_varint(loraSub, loraLen, &loraPos, &loraKey)) break;
                int loraField = (int)(loraKey >> 3);
                WireType loraWt = (WireType)(loraKey & 0x7);

                if (loraField == 2 && loraWt == WT_VARINT)  // modem_preset (explicit)
                {
                    unsigned __int64 v = 0;
                    if (pb_read_varint(loraSub, loraLen, &loraPos, &v))
                    {
                        devCfg->modemPreset = (int)v;
                        devCfg->modemPresetPresent = true;
                        char msg[64];
                        sprintf(msg, "DEBUG: Got modem_preset=%d", (int)v);
                        AppendSerialLog(msg, false);
                    }
                }
                else if (loraField == 3 && loraWt == WT_VARINT)  // bandwidth
                {
                    unsigned __int64 v = 0;
                    if (pb_read_varint(loraSub, loraLen, &loraPos, &v))
                    {
                        devCfg->bandwidth = (int)v;
                        char msg[64];
                        sprintf(msg, "DEBUG: Got bandwidth=%d", (int)v);
                        AppendSerialLog(msg, false);
                    }
                }
                else if (loraField == 4 && loraWt == WT_VARINT)  // spread_factor
                {
                    unsigned __int64 v = 0;
                    if (pb_read_varint(loraSub, loraLen, &loraPos, &v))
                    {
                        devCfg->spreadFactor = (int)v;
                        char msg[64];
                        sprintf(msg, "DEBUG: Got spread_factor=%d", (int)v);
                        AppendSerialLog(msg, false);
                    }
                }
                else if (loraField == 5 && loraWt == WT_VARINT)  // coding_rate
                {
                    unsigned __int64 v = 0;
                    if (pb_read_varint(loraSub, loraLen, &loraPos, &v))
                    {
                        devCfg->codingRate = (int)v;
                        char msg[64];
                        sprintf(msg, "DEBUG: Got coding_rate=%d", (int)v);
                        AppendSerialLog(msg, false);
                    }
                }
                else if (loraField == 7 && loraWt == WT_VARINT)  // region
                {
                    unsigned __int64 v = 0;
                    if (pb_read_varint(loraSub, loraLen, &loraPos, &v))
                    {
                        devCfg->region = (int)v;
                        char msg[64];
                        sprintf(msg, "DEBUG: Got region=%d", (int)v);
                        AppendSerialLog(msg, false);
                    }
                }
                else if (loraField == 8 && loraWt == WT_VARINT)  // hop_limit
                {
                    unsigned __int64 v = 0;
                    if (pb_read_varint(loraSub, loraLen, &loraPos, &v))
                    {
                        devCfg->hopLimit = (int)v;
                        char msg[64];
                        sprintf(msg, "DEBUG: Got hop_limit=%d", (int)v);
                        AppendSerialLog(msg, false);
                    }
                }
                else if (loraField == 9 && loraWt == WT_VARINT)  // tx_enabled
                {
                    unsigned __int64 v = 0;
                    if (pb_read_varint(loraSub, loraLen, &loraPos, &v))
                    {
                        devCfg->txEnabled = (v != 0);
                        devCfg->txEnabledPresent = true;
                        char msg[64];
                        sprintf(msg, "DEBUG: Got tx_enabled=%d", (int)v);
                        AppendSerialLog(msg, false);
                    }
                }
                else if (loraField == 10 && loraWt == WT_VARINT)  // tx_power
                {
                    unsigned __int64 v = 0;
                    if (pb_read_varint(loraSub, loraLen, &loraPos, &v))
                    {
                        devCfg->txPower = (int)v;
                        char msg[64];
                        sprintf(msg, "DEBUG: Got tx_power=%d dBm", (int)v);
                        AppendSerialLog(msg, false);
                    }
                }
                else if (loraField == 11 && loraWt == WT_VARINT)  // channel_num (frequency slot)
                {
                    unsigned __int64 v = 0;
                    if (pb_read_varint(loraSub, loraLen, &loraPos, &v))
                    {
                        devCfg->freqSlot = (int)v;
                        char msg[64];
                        sprintf(msg, "DEBUG: Got channel_num=%d", (int)v);
                        AppendSerialLog(msg, false);
                    }
                }
                else
                {
                    LogUnknownField("LORA", loraField, (int)loraWt);
                    if (!pb_skip_field(loraWt, loraSub, loraLen, &loraPos)) break;
                }
            }

            if (devCfg->modemPresetPresent) {
                char msg2[64];
                sprintf(msg2, "DEBUG: Using modem_preset=%d (explicit)", devCfg->modemPreset);
                AppendSerialLog(msg2, false);
            } else {
                AppendSerialLog("DEBUG: modem_preset not present - may fall back to BW/SF detection", false);
            }
            
            // After parsing all LoRa config fields, detect preset from radio params only if modem_preset was NOT provided.
            // NOTE: This is best-effort; if firmware doesn't send modem_preset we infer based on BW/SF/CR.
            if (!devCfg->modemPresetPresent && devCfg->bandwidth > 0 && devCfg->spreadFactor > 0)
            {
                char pmsg[128];
                sprintf(pmsg, "DEBUG: Preset inference from params: BW=%d SF=%d CR=%d", devCfg->bandwidth, devCfg->spreadFactor, devCfg->codingRate);
                AppendSerialLog(pmsg, false);

                // Common Meshtastic defaults (as seen in official clients):
                //  - Long Range:   BW=250, SF=11 (CR differentiates Fast/Moderate/Slow)
                //  - Medium Range: BW=250, SF=10 (CR differentiates Fast/Slow)
                //  - Short Range:  BW=250, SF=7/8 and Turbo uses BW=500
                if (devCfg->bandwidth == 250 && devCfg->spreadFactor == 11)
                {
                    // CR mapping (Meshtastic uses numeric CR values like 5 (4/5), 6 (4/6), 8 (4/8))
                    if (devCfg->codingRate == 8)
                    {
                        devCfg->modemPreset = MODEM_PRESET_LONG_SLOW;
                        AppendSerialLog("DEBUG: Detected preset: Long Range - Slow (BW=250, SF=11, CR=8)", false);
                    }
                    else if (devCfg->codingRate == 6)
                    {
                        devCfg->modemPreset = MODEM_PRESET_LONG_MODERATE;
                        AppendSerialLog("DEBUG: Detected preset: Long Range - Moderate (BW=250, SF=11, CR=6)", false);
                    }
                    else
                    {
                        // Most firmwares report Long Fast with CR=5
                        devCfg->modemPreset = MODEM_PRESET_LONG_FAST;
                        AppendSerialLog("DEBUG: Detected preset: Long Range - Fast (BW=250, SF=11)", false);
                    }
                }
                else if (devCfg->bandwidth == 250 && devCfg->spreadFactor == 10)
                {
                    if (devCfg->codingRate == 8)
                    {
                        devCfg->modemPreset = MODEM_PRESET_MEDIUM_SLOW;
                        AppendSerialLog("DEBUG: Detected preset: Medium Range - Slow (BW=250, SF=10, CR=8)", false);
                    }
                    else
                    {
                        devCfg->modemPreset = MODEM_PRESET_MEDIUM_FAST;
                        AppendSerialLog("DEBUG: Detected preset: Medium Range - Fast (BW=250, SF=10)", false);
                    }
                }
                else if (devCfg->bandwidth == 500 && devCfg->spreadFactor == 7)
                {
                    devCfg->modemPreset = MODEM_PRESET_SHORT_TURBO;
                    AppendSerialLog("DEBUG: Detected preset: Short Range - Turbo (BW=500, SF=7)", false);
                }
                else if (devCfg->bandwidth == 250 && devCfg->spreadFactor == 7)
                {
                    devCfg->modemPreset = MODEM_PRESET_SHORT_FAST;
                    AppendSerialLog("DEBUG: Detected preset: Short Range - Fast (BW=250, SF=7)", false);
                }
                else if (devCfg->bandwidth == 250 && devCfg->spreadFactor == 8)
                {
                    devCfg->modemPreset = MODEM_PRESET_SHORT_SLOW;
                    AppendSerialLog("DEBUG: Detected preset: Short Range - Slow (BW=250, SF=8)", false);
                }
                else
                {
                    char msg[140];
                    sprintf(msg, "DEBUG: Unknown preset params (BW=%d, SF=%d, CR=%d) - leaving preset unchanged", 
                            devCfg->bandwidth, devCfg->spreadFactor, devCfg->codingRate);
                    AppendSerialLog(msg, false);
                }
            }

            if (!devCfg->txEnabledPresent)
            {
                AppendSerialLog("DEBUG: tx_enabled not present -> default FALSE(0)", false);
            }

            pos += (int)l;
        }
        else
        {
            LogUnknownField("CONFIG", field, (int)wt);
            if (!pb_skip_field(wt, payload, payloadLen, &pos)) break;
        }
    }
}

static void HandleFromRadio(const unsigned char* payload, int payloadLen)
{
    // FromRadio fields:
    // packet=2 (len)
    // my_info=3 (len)
    // node_info=4 (len)
    // config=5 (len)
    // config_complete_id=7 (varint)
    // heartbeat=11 (len)  (response to ToRadio.heartbeat)
    int pos = 0;
    while (pos < payloadLen)
    {
        unsigned __int64 key = 0;
        if (!pb_read_varint(payload, payloadLen, &pos, &key)) break;
        int field = (int)(key >> 3);
        WireType wt = (WireType)(key & 0x7);

        if ((field == 2 || field == 3 || field == 4 || field == 5) && wt == WT_LEN)
        {
            unsigned __int64 l = 0;
            if (!pb_read_varint(payload, payloadLen, &pos, &l)) break;
            if (pos + (int)l > payloadLen) break;

            const unsigned char* sub = payload + pos;
            int subLen = (int)l;

            if (field == 2)
                HandleMeshPacket(sub, subLen);
            else if (field == 3)
                HandleMyInfo(sub, subLen);
            else if (field == 4)
                HandleNodeInfo(sub, subLen);
            else if (field == 5)
                HandleConfig(sub, subLen);

            pos += (int)l;
        }
        else if (field == 11 && wt == WT_LEN)
        {
            // Heartbeat response from device. We don't currently use its contents,
            // but we must NOT spam "Unknown field" logs or it will look like a stuck loop.
            unsigned __int64 l = 0;
            if (!pb_read_varint(payload, payloadLen, &pos, &l)) break;
            if (pos + (int)l > payloadLen) break;
            // (Optional) could parse uptime/metrics here later.
            pos += (int)l;

            // Fallback: if we requested settings (hourglass is on) and we already received some
            // config chunks, but never got config_complete_id, treat the first heartbeat after
            // those chunks as a "good enough" completion signal.
            if (g_App.isFetchingSettings && g_ConfigChunksSeenWhileFetching)
            {
                AppendSerialLog("DEBUG: Heartbeat after config chunks -> finishing settings fetch (fallback)", false);
                g_ConfigChunksSeenWhileFetching = false;
                Settings_OnDeviceConfigReceived();
            }
        }
        else if ((field == 7 || field == 8) && wt == WT_VARINT)
        {
            unsigned __int64 v = 0;
            if (!pb_read_varint(payload, payloadLen, &pos, &v)) break;
            
            char dbgMsg[128];
            sprintf(dbgMsg, "DEBUG: Received config_complete_id (field 7/8), nonce=%u (expecting %u or %u)", 
                    (DWORD)v, g_LastWantConfigNonceSent, g_WantConfigNonce);
            AppendSerialLog(dbgMsg, false);
            
            // Check against current nonce OR previous nonce (we increment before receiving response)
            if (g_LastWantConfigNonceSent != 0 && (DWORD)v == g_LastWantConfigNonceSent)
            {
                AppendSerialLog("Config complete.", false);
                AppendSerialLog("DEBUG: Nonce matches! Calling Settings_OnDeviceConfigReceived()...", false);
                // Update UI with device settings
                Settings_OnDeviceConfigReceived();
                AppendSerialLog("DEBUG: Returned from Settings_OnDeviceConfigReceived()", false);
            }
            else
            {
                AppendSerialLog("DEBUG: Nonce mismatch - ignoring config_complete", false);
            }
        }
        else
        {
            // Unknown FromRadio field. Keep this log lightweight.
            LogUnknownField("FROMRADIO", field, (int)wt);
            if (!pb_skip_field(wt, payload, payloadLen, &pos)) break;
        }
    }
}

// ------------------------------------------------------------
// Framing + TX
// ------------------------------------------------------------

static bool SendToRadioPayload(const unsigned char* toRadioPayload, int toRadioLen)
{
    // Frame: 0x94 0xC3 len_hi len_lo payload
    unsigned char frame[2048];
    if (toRadioLen < 0 || toRadioLen > (int)sizeof(frame) - 4) return false;
    frame[0] = FRAME_MAGIC1;
    frame[1] = FRAME_MAGIC2;
    frame[2] = (unsigned char)((toRadioLen >> 8) & 0xFF);
    frame[3] = (unsigned char)(toRadioLen & 0xFF);
    memcpy(frame + 4, toRadioPayload, toRadioLen);

    LogFrame("TX", toRadioPayload, toRadioLen);
    return Serial_SendBytes(frame, toRadioLen + 4);
}

bool Meshtastic_RequestConfig()
{
    // Only the most recent want_config request should trigger UI refresh.
    // We track the nonce we actually sent and match config_complete against it.
    char dbgMsg[160];

    DWORD nonce = g_WantConfigNonce;

    // New fetch cycle.
    g_ConfigChunksSeenWhileFetching = false;
    sprintf(dbgMsg, "DEBUG: Meshtastic_RequestConfig() called, sending nonce=%u", nonce);
    AppendSerialLog(dbgMsg, false);

    // ToRadio.want_config_id = 3
    unsigned char pb[64];
    int n = 0;
    pb_write_u32(3, nonce, pb, &n, sizeof(pb));

    sprintf(dbgMsg, "DEBUG: Built want_config payload, size=%d bytes", n);
    AppendSerialLog(dbgMsg, false);

    g_LastWantConfigNonceSent = nonce;
    g_WantConfigNonce++;

    bool result = SendToRadioPayload(pb, n);
    sprintf(dbgMsg, "DEBUG: SendToRadioPayload() returned %s", result ? "TRUE" : "FALSE");
    AppendSerialLog(dbgMsg, false);

    // Also ask for admin session key so we can later send settings.
    // This is a separate message on ADMIN_APP and will be handled asynchronously.
    Admin_RequestOwnerPasskeyIfNeeded();

    return result;
}

static int BuildDeviceConfigPb(const DeviceSettings* s, unsigned char* out, int maxLen)
{
    int n = 0;
    if (!s) return 0;
    // DeviceConfig.role = 1 (varint)
    pb_write_u32(1, (DWORD)s->role, out, &n, maxLen);
    return n;
}

static int BuildPositionConfigPb(const DeviceSettings* s, unsigned char* out, int maxLen)
{
    int n = 0;
    if (!s) return 0;
    // PositionConfig.gps_mode = 13 (varint)
    pb_write_u32(13, (DWORD)s->gpsMode, out, &n, maxLen);
    return n;
}

static int BuildLoRaConfigPb(const DeviceSettings* s, unsigned char* out, int maxLen)
{
    int n = 0;
    if (!s) return 0;

    // FORCED PRESET MODE
    // Since we only need presets, we force use_preset=true (1) and clear all custom PHY params.
    // This works for all presets including LONG_FAST (0).
    bool usePreset = true;

    // [FIX 1] Send 'use_preset' (Field 1). This is the master switch for the firmware.
    pb_write_bool(1, usePreset, out, &n, maxLen);

    // LoRaConfig.modem_preset = 2
    pb_write_u32(2, (DWORD)s->modemPreset, out, &n, maxLen);

    // [FIX 2] Explicitly write 0 for PHY params to ensure firmware uses the preset definitions.
    pb_write_u32(3, 0, out, &n, maxLen);      // bandwidth
    pb_write_u32(4, 0, out, &n, maxLen);      // spread_factor
    pb_write_u32(5, 0, out, &n, maxLen);      // coding_rate

    // Write remaining fields
    pb_write_u32(7, (DWORD)s->region, out, &n, maxLen);
    pb_write_u32(8, (DWORD)s->hopLimit, out, &n, maxLen);
    pb_write_u32(9, (DWORD)(s->txEnabled ? 1 : 0), out, &n, maxLen);
    pb_write_u32(10, (DWORD)s->txPower, out, &n, maxLen);
    pb_write_u32(11, (DWORD)s->freqSlot, out, &n, maxLen);
    return n;
}

static int BuildConfigPb(const DeviceSettings* s, unsigned char* out, int maxLen)
{
    if (!s) return 0;

    unsigned char devPb[64];
    int dn = BuildDeviceConfigPb(s, devPb, sizeof(devPb));

    unsigned char posPb[128];
    int pn = BuildPositionConfigPb(s, posPb, sizeof(posPb));

    unsigned char loraPb[128];
    int ln = BuildLoRaConfigPb(s, loraPb, sizeof(loraPb));

    int n = 0;
    if (dn > 0) pb_write_bytes(1, devPb, dn, out, &n, maxLen);   // Config.device
    if (pn > 0) pb_write_bytes(2, posPb, pn, out, &n, maxLen);   // Config.position
    if (ln > 0) pb_write_bytes(6, loraPb, ln, out, &n, maxLen);  // Config.lora
    return n;
}

static bool SendAdminWithPasskey(const unsigned char* adminPayload, int adminLen)
{
    if (g_AdminPasskeyLen <= 0)
        return SendAdminPayload(adminPayload, adminLen);

    // Wrap with session_passkey if not already included.
    unsigned char buf[900];
    int n = 0;
    pb_write_bytes(101, g_AdminPasskey, g_AdminPasskeyLen, buf, &n, sizeof(buf));
    // Instead of trying to detect, just prepend passkey and then append original message fields.
    // Because protobuf allows repeated/duplicate fields, the later fields win.
    if (n + adminLen > (int)sizeof(buf)) return false;
    memcpy(buf + n, adminPayload, adminLen);
    n += adminLen;
    return SendAdminPayload(buf, n);
}

bool Meshtastic_SendDeviceSettings(const DeviceSettings* settings)
{
    if (!Serial_IsOpen() || !settings)
        return false;

    // Build directly from settings. 
    // BuildLoRaConfigPb() now handles the logic of forcing 0s if a preset is selected.
    
    // Ensure we have (or are requesting) a passkey.
    Admin_RequestOwnerPasskeyIfNeeded();

    // Build AdminMessage.begin_edit_settings = 64 (bool)
    {
        unsigned char adminPb[64];
        int an = 0;
        pb_write_bool(64, true, adminPb, &an, sizeof(adminPb));
        SendAdminWithPasskey(adminPb, an);
    }

    // Build AdminMessage.set_config = 34 (Config)
    {
        unsigned char cfgPb[512];
        int cn = BuildConfigPb(settings, cfgPb, sizeof(cfgPb));

        unsigned char adminPb[768];
        int an = 0;
        pb_write_bytes(34, cfgPb, cn, adminPb, &an, sizeof(adminPb));
        SendAdminWithPasskey(adminPb, an);
    }

    // Build AdminMessage.commit_edit_settings = 65 (bool)
    {
        unsigned char adminPb[64];
        int an = 0;
        pb_write_bool(65, true, adminPb, &an, sizeof(adminPb));
        SendAdminWithPasskey(adminPb, an);
    }

    AppendSerialLog("DEBUG: Sent device settings via ADMIN_APP (begin/set_config/commit)", false);
    return true;
}

bool Meshtastic_SendTextEx(DWORD nodeNum, const char* pszText, DWORD* outPacketId)
{
    if (!Serial_IsOpen() || !pszText || !pszText[0])
        return false;

    // Generate a non-zero MeshPacket.id so we can match Routing acks (Data.request_id).
    // (Old firmwares might generate an id if we leave it empty, but this is more reliable.)
    DWORD pktId = (DWORD)(GetTickCount() ^ (DWORD)((DWORD)rand() << 16));
    if (pktId == 0) pktId = 1;
    if (outPacketId) *outPacketId = pktId;

    // Build Data { portnum=1, payload=<utf8> }
    unsigned char dataPb[600];
    int dn = 0;
    pb_write_u32(1, 1, dataPb, &dn, sizeof(dataPb)); // TEXT_MESSAGE_APP
    pb_write_bytes(2, (const unsigned char*)pszText, (int)strlen(pszText), dataPb, &dn, sizeof(dataPb));

    // Build MeshPacket { to=..., channel=0, id=<pktId>, decoded=data }
    unsigned char packetPb[700];
    int pn = 0;
    DWORD to = (nodeNum == 0) ? BROADCAST_ADDR : nodeNum;
    pb_write_key(2, WT_32BIT, packetPb, &pn, sizeof(packetPb));
    pb_write_fixed32(to, packetPb, &pn, sizeof(packetPb));
    pb_write_u32(3, 0, packetPb, &pn, sizeof(packetPb));
    pb_write_key(6, WT_32BIT, packetPb, &pn, sizeof(packetPb));
    pb_write_fixed32(pktId, packetPb, &pn, sizeof(packetPb));
    pb_write_bytes(4, dataPb, dn, packetPb, &pn, sizeof(packetPb));
    pb_write_bool(10, true, packetPb, &pn, sizeof(packetPb)); // want_ack

    // Build ToRadio { packet=1 }
    unsigned char toRadioPb[900];
    int tn = 0;
    pb_write_bytes(1, packetPb, pn, toRadioPb, &tn, sizeof(toRadioPb));

    return SendToRadioPayload(toRadioPb, tn);
}

void Meshtastic_SendHeartbeat()
{
    if (!Serial_IsOpen()) return;
    // ToRadio.heartbeat = 7, Heartbeat.nonce = 1
    unsigned char hbPb[16];
    int hn = 0;
    pb_write_u32(1, GetTickCount(), hbPb, &hn, sizeof(hbPb));

    unsigned char toRadioPb[32];
    int tn = 0;
    pb_write_bytes(7, hbPb, hn, toRadioPb, &tn, sizeof(toRadioPb));
    SendToRadioPayload(toRadioPb, tn);
}

static void ProcessFrames()
{
    // Consume frames from g_InBuf
    while (g_InLen >= 4)
    {
        // Find magic
        int start = -1;
        for (int i = 0; i < g_InLen - 1; i++)
        {
            if (g_InBuf[i] == FRAME_MAGIC1 && g_InBuf[i+1] == FRAME_MAGIC2)
            {
                start = i;
                break;
            }
        }
        if (start < 0)
        {
            g_InLen = 0;
            return;
        }
        if (start > 0)
        {
            memmove(g_InBuf, g_InBuf + start, g_InLen - start);
            g_InLen -= start;
        }
        if (g_InLen < 4) return;

        int payloadLen = ((int)g_InBuf[2] << 8) | (int)g_InBuf[3];
        if (payloadLen < 0 || payloadLen > (int)sizeof(g_InBuf) - 4)
        {
            // Bad length - resync by dropping first byte
            memmove(g_InBuf, g_InBuf + 1, g_InLen - 1);
            g_InLen -= 1;
            continue;
        }
        if (g_InLen < 4 + payloadLen) return;

        const unsigned char* payload = g_InBuf + 4;
        LogFrame("RX", payload, payloadLen);
        HandleFromRadio(payload, payloadLen);

        // Consume
        int consume = 4 + payloadLen;
        memmove(g_InBuf, g_InBuf + consume, g_InLen - consume);
        g_InLen -= consume;
    }
}

// ------------------------------------------------------------
// Public entry points
// ------------------------------------------------------------

void Meshtastic_Init()
{
    Serial_Init(Meshtastic_OnSerialBytes);
}

void Meshtastic_OnConnected()
{
    // Ask for node DB/config on connect.
    AppendSerialLog("Requesting config (want_config)...", false);
    Meshtastic_RequestConfig();
}

void Meshtastic_OnDisconnected()
{
    AppendSerialLog("Disconnected.", false);
}

void Meshtastic_OnSerialBytes(const unsigned char* pData, int len, bool isRx)
{
    if (!isRx)
        return;

    if (!pData || len <= 0)
        return;

    // Append to input buffer
    int space = (int)sizeof(g_InBuf) - g_InLen;
    if (len > space)
    {
        // Drop oldest to make room
        int drop = len - space;
        if (drop > g_InLen) drop = g_InLen;
        if (drop > 0)
        {
            memmove(g_InBuf, g_InBuf + drop, g_InLen - drop);
            g_InLen -= drop;
        }
    }

    if (len > (int)sizeof(g_InBuf) - g_InLen)
        len = (int)sizeof(g_InBuf) - g_InLen;

    memcpy(g_InBuf + g_InLen, pData, len);
    g_InLen += len;

    ProcessFrames();
}

