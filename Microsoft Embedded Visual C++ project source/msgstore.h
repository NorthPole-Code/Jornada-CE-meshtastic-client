#pragma once
#include <windows.h>

// Message persistence layer.
// Stores chat + DM histories under: <App storagePath>\Messages\
// Designed for WinCE / eVC++ 3.0 (no STL).

// Ensure storage directories exist (best-effort).
void MsgStore_Init();

// Load full broadcast chat history into the provided edit control (replaces its contents).
void MsgStore_LoadChatHistory(HWND hChatHistory);

// Append a chat line (timestamp + prefix + text) to UI and persist to disk.
void MsgStore_AppendChat(HWND hChatHistory, LPCTSTR prefix, LPCTSTR text);

// Append a chat line with a fixed-width marker appended right before CRLF.
// Returns the TCHAR index in the edit control where the 3-char marker begins (or -1 on failure).
int MsgStore_AppendChatWithMarker(HWND hChatHistory, LPCTSTR prefix, LPCTSTR text, LPCTSTR marker3);

// Persist a single DM line (already formatted, includes CRLF) for nodeNum.
void MsgStore_AppendDMLine(DWORD nodeNum, LPCTSTR line);

// Overwrite the entire DM history file for nodeNum with the provided text.
// (Used for updating ACK markers after the line was initially persisted.)
void MsgStore_OverwriteDMHistory(DWORD nodeNum, LPCTSTR fullText);

// Load DM history for nodeNum into out buffer (null-terminated). Returns true if file existed.
bool MsgStore_LoadDMHistory(DWORD nodeNum, LPTSTR out, int cchOut);

// Enumerate node numbers that have a DM history file.
typedef void (*MSGSTORE_ENUM_DM_CB)(DWORD nodeNum, void* ctx);
void MsgStore_EnumDMConversations(MSGSTORE_ENUM_DM_CB cb, void* ctx);
