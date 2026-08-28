#pragma once

#define NODRAWTEXT    
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dwmapi.h>
#include <DispatcherQueue.h>
#include <unknwn.h>

#include <d2d1_1.h>
#include <dwrite.h>
#include <d3d11_1.h>

// Подключаем С++/WinRT
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.Composition.Desktop.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <windows.ui.composition.interop.h>

