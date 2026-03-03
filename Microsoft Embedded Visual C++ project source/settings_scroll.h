#pragma once

#include <windows.h>

// Initialize settings scroll handling
void SettingsScroll_Init(HWND hPage);

// Handle scroll messages
void SettingsScroll_OnVScroll(HWND hPage, WPARAM wParam);

// Update scroll info after resize
void SettingsScroll_UpdateInfo(HWND hPage);
