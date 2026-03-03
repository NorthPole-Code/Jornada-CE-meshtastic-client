#pragma once
#include <windows.h>

#define MAPVIEW_CLASS TEXT("MeshtasticMapView")

// Register the map view window class once at startup
void MapView_Register(HINSTANCE hInst);
