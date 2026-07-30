#pragma once

// Must be first: required by GDI+ for IStream, IUnknown etc.
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <commctrl.h>
#include <dwmapi.h>

// OLE / COM must come before GDI+
#include <objbase.h>
#include <objidl.h>
#include <ole2.h>
#include <oleidl.h>
#include <shobjidl.h>
#include <wrl/client.h>

// GDI+
#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")

// DirectX / DXGI
#include <d3d11.h>
#include <dxgi1_2.h>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

// Standard Library
#include <string>
#include <vector>
#include <functional>
#include <mutex>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cmath>
#include <ctime>
#include <chrono>
