#pragma once

#include <windows.h>

// For DeviceSettings
#include "config.h"

// Meshtastic serial protocol handler (protobuf framing over UART)
// Implements enough of the Meshtastic "Client API" to support:
// - Requesting node DB (want_config)
// - Receiving NodeInfo + Position updates
// - Sending/receiving chat messages (broadcast + direct)

void Meshtastic_Init();

// Call when a serial link has been opened/closed
void Meshtastic_OnConnected();
void Meshtastic_OnDisconnected();

// Raw serial bytes callback (wired up by Serial_Init)
void Meshtastic_OnSerialBytes(const unsigned char* pData, int len, bool isRx);

// Register an outgoing broadcast-chat line for ACK marker updates.
// markerPos is the TCHAR index inside g_App.hChatHistory where the 3-char marker begins.
void Meshtastic_RegisterChatOutgoing(DWORD packetId, int markerPos);

// Send a text message
// nodeNum == 0 => broadcast on primary channel
// If outPacketId is non-NULL, it will receive the MeshPacket.id used for this message.
// That ID can be matched against Routing acks (decoded.request_id).
bool Meshtastic_SendTextEx(DWORD nodeNum, const char* pszText, DWORD* outPacketId);

// Backwards-compatible helper
inline bool Meshtastic_SendText(DWORD nodeNum, const char* pszText)
{
    return Meshtastic_SendTextEx(nodeNum, pszText, NULL);
}

// Ask radio to send node DB + config chunks
bool Meshtastic_RequestConfig();

// Send updated settings back to the device (via ADMIN_APP / AdminMessage).
// The radio might reboot after committing settings.
bool Meshtastic_SendDeviceSettings(const DeviceSettings* settings);

// Keep serial connection alive (optional but recommended)
void Meshtastic_SendHeartbeat();
void Meshtastic_TickAckBlink();

