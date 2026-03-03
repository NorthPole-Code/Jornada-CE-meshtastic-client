#pragma once
#include "app.h"

#define MAX_NODES 128

void Nodes_Init();
void Nodes_RebuildList();
void Nodes_OnSearchChange();
void Nodes_OnChooseStoragePath(HWND hwndOwner);
void Nodes_OnColumnClick(int column);

NodeInfo* Nodes_FindOrAdd(DWORD nodeNum);
int Nodes_GetCount();
NodeInfo* Nodes_GetAt(int index);

// New: expose storage file path & storage directory (for Maps folder)
void Nodes_GetStorageFilePath(LPTSTR out, int cchOut);
void Nodes_GetStorageDir(LPTSTR out, int cchOut);

// Display helpers: if a node doesn't report short/long name, use last 4 chars of its USER ID
// (same behavior as Meshtastic Android app).
void Nodes_GetDisplayShortName(const NodeInfo* n, LPTSTR out, int cchOut);
void Nodes_GetDisplayLongName(const NodeInfo* n, LPTSTR out, int cchOut);


// --------------------------------------------------
// Direct message conversations (Direct tab)
// Only nodes explicitly added (via Node Details button)
// or nodes that have sent us a DM should appear here.
// --------------------------------------------------
void Direct_Init();
void Direct_AddConversation(DWORD nodeNum);
void Direct_OnIncomingText(DWORD fromNode, const char* utf8Text);
void Direct_OnOutgoingText(DWORD toNode, LPCTSTR localText);

// Append an outgoing DM line with an initial " - " marker and register it for ACK updates.
void Direct_OnOutgoingTextWithAck(DWORD toNode, LPCTSTR localText, DWORD packetId);

// Outgoing DM ACK tracking (used for rendering " - ", " + ", "++ " at line end)
// markerPos is a TCHAR index into the convo's historyRaw string where the 3-char marker begins.
void Direct_RegisterOutgoingAck(DWORD packetId, DWORD toNode, int markerPos);
// Called by the protocol layer when a Routing ack/nak is received for requestId.
void Direct_OnRoutingAck(DWORD ackFromNode, DWORD requestId, bool success);
void Direct_TickAckBlink();
void Direct_OnSelectionChanged();
DWORD Direct_GetSelectedNode();

// Re-render the currently selected DM history using the current
// timestamp display mode (time-only vs date+time).
void Direct_RefreshTimestampDisplay();

// Unread helpers for Direct tab
int  Direct_GetUnreadCount(DWORD nodeNum);
int  Direct_GetTotalUnread();
int  Direct_ClearUnread(DWORD nodeNum); // returns number cleared
void Direct_IncUnread(DWORD nodeNum);