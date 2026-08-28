#include "pch.h"

#pragma comment(lib, "windowsapp.lib")
#pragma comment(lib, "coremessaging.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "d3d11.lib")

using namespace winrt;
using namespace Windows::UI::Composition;
using namespace Windows::UI::Composition::Desktop;
using namespace Windows::System;
using namespace winrt::Windows::Foundation::Numerics;

static Compositor g_compositor { nullptr };
static DesktopWindowTarget g_target { nullptr };
static DispatcherQueueController g_queueController { nullptr };

static ContainerVisual g_rootVisual { nullptr };
static SpriteVisual g_backgroundVisual { nullptr };

static ID2D1Factory1 * g_d2dFactory { nullptr };
static IDWriteFactory * g_dwriteFactory { nullptr };
static CompositionGraphicsDevice g_graphicsDevice { nullptr };
static SpriteVisual g_textVisual { nullptr }; // Визуал, который будет содержать текст

static CompositionSpriteShape g_buttonShape { nullptr };
static bool g_isMouseOver { false }; // Флаг: находится ли мышь над кнопкой сейчас

static ContainerVisual g_buttonContainer { nullptr }; // Общий родитель для всей кнопки с тенью
static ShapeVisual     g_buttonBg { nullptr };        // Сама синяя кнопка (теперь ShapeVisual)
static SpriteVisual    g_shadowVisual { nullptr };    // Визуал, который держит тень
static DropShadow      g_buttonShadow { nullptr };    // Объект тени

// Константы цветов кнопки
const winrt::Windows::UI::Color BORDER_NORMAL { 128, 0, 120, 212 }; // Стандартный синий
const winrt::Windows::UI::Color COLOR_NORMAL { 255, 233, 233, 255 }; // Стандартный синий
const winrt::Windows::UI::Color COLOR_HOVER { 255, 212, 212, 255 }; // Светло-синий при наведении

using Point = winrt::Windows::Foundation::Point;

struct Offset : public Point { 
    Offset(float x = 0, float y = 0) : Point { x, y } {}
    operator float3() {
        return float3 { X, Y, 0.0f };
    }
};

using Size = winrt::Windows::Foundation::Size;

Offset operator+(Offset a, Offset b) { return { a.X + b.X, a.Y + b.Y }; }
Offset operator-(Offset a, Offset b) { return { a.X - b.X, a.Y - b.Y }; }

//Offset operator+(Offset a, Size b) { return { a.X + b.Width, a.Y + b.Height }; }
//Offset operator-(Offset a, Size b) { return { a.X - b.Width, a.Y - b.Height }; }

Size operator+(Size a, Size b) { return { a.Width + b.Width, a.Height + b.Height }; }
Size operator-(Size a, Size b) { return { a.Width - b.Width, a.Height - b.Height }; }

Offset buttonPos = {};
Size buttonSize = {120.0f, 35.0f};

LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

void EnsureDispatcherQueue() {
    if(g_queueController) return;
    DispatcherQueueOptions options { sizeof(DispatcherQueueOptions), DQTYPE_THREAD_CURRENT, DQTAT_COM_STA };
    ABI::Windows::System::IDispatcherQueueController * ptr { nullptr };
    winrt::check_hresult(CreateDispatcherQueueController(options, &ptr));
    copy_from_abi(g_queueController, ptr);
}

void InitWinRTComposition(HWND hWnd) {
    EnsureDispatcherQueue();
    g_compositor = Compositor();

    // 1. Инициализация Direct3D 11
    ID3D11Device * d3dDevice { nullptr };
    ID3D11DeviceContext * d3dContext { nullptr };
    D3D_FEATURE_LEVEL featureLevel;

    HRESULT hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr, 0,
        D3D11_SDK_VERSION,
        &d3dDevice, &featureLevel, &d3dContext
    );
    winrt::check_hresult(hr);

    // 2. Получаем DXGI интерфейс устройства
    IDXGIDevice * dxgiDevice { nullptr };
    hr = d3dDevice->QueryInterface(__uuidof(IDXGIDevice), (void **)&dxgiDevice);
    winrt::check_hresult(hr);

    // 3. Создаем Direct2D устройство
    ID2D1Device * d2dDevice { nullptr };
    D2D1_CREATION_PROPERTIES props = { D2D1_THREADING_MODE_SINGLE_THREADED, D2D1_DEBUG_LEVEL_NONE, D2D1_DEVICE_CONTEXT_OPTIONS_NONE };
    hr = D2D1CreateDevice(dxgiDevice, &props, &d2dDevice);
    winrt::check_hresult(hr);

    // Инициализируем Direct2D фабрику
    hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), nullptr, (void **)&g_d2dFactory);
    winrt::check_hresult(hr);

    // Инициализируем DirectWrite фабрику (ОБЯЗАТЕЛЬНО проверяем hr!)
    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown **)&g_dwriteFactory);
    winrt::check_hresult(hr);

    // 4. Связываем графику с WinRT Композитором
    auto interopCompositor = g_compositor.as<ABI::Windows::UI::Composition::ICompositorInterop>();
    IUnknown * deviceUnknown = nullptr;
    d2dDevice->QueryInterface(__uuidof(IUnknown), (void **)&deviceUnknown);

    ABI::Windows::UI::Composition::ICompositionGraphicsDevice * geoDevicePtr { nullptr };
    interopCompositor->CreateGraphicsDevice(deviceUnknown, &geoDevicePtr);
    winrt::copy_from_abi(g_graphicsDevice, geoDevicePtr);

    // Освобождаем локальные DX интерфейсы
    if(deviceUnknown) deviceUnknown->Release();
    if(d2dDevice) d2dDevice->Release();
    if(dxgiDevice) dxgiDevice->Release();
    if(d3dContext) d3dContext->Release();
    if(d3dDevice) d3dDevice->Release();

    // =========================================================================
    // ВАЖНО: ВОЗВРАЩАЕМ ПРИВЯЗКУ К ОКНУ (Этот блок у вас пропал)
    // =========================================================================
    auto interop = g_compositor.as<ABI::Windows::UI::Composition::Desktop::ICompositorDesktopInterop>();
    ABI::Windows::UI::Composition::Desktop::IDesktopWindowTarget * targetPtr { nullptr };

    // Передаем true: WinRT берет на себя отрисовку всего контента HWND
    winrt::check_hresult(interop->CreateDesktopWindowTarget(hWnd, true, &targetPtr));
    copy_from_abi(g_target, targetPtr);
}

CompositionSurfaceBrush CreateTextBrush(const wchar_t * text, Size size) {
    // 1. Создаем виртуальную поверхность внутри WinRT Composition
    CompositionDrawingSurface surface = g_graphicsDevice.CreateDrawingSurface(
        size,
        winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
        winrt::Windows::Graphics::DirectX::DirectXAlphaMode::Premultiplied);

    // 2. Начинаем отрисовку через Interop-интерфейс
    auto surfaceInterop = surface.as<ABI::Windows::UI::Composition::ICompositionDrawingSurfaceInterop>();
    ID2D1DeviceContext * d2dContext { nullptr };
    POINT offset;
    winrt::check_hresult(surfaceInterop->BeginDraw(nullptr, __uuidof(ID2D1DeviceContext), (void **)&d2dContext, &offset));

    // Настраиваем трансформацию с учетом смещения окна
    d2dContext->SetTransform(D2D1::Matrix3x2F::Translation(static_cast<float>(offset.x), static_cast<float>(offset.y)));
    d2dContext->Clear(D2D1::ColorF(0, 0, 0, 0)); // Прозрачный фон для текста

    // 3. Создаем формат текста (Шрифт, Размер, Выравнивание)
    IDWriteTextFormat * textFormat { nullptr };
    g_dwriteFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 16.0f, L"ru-ru", &textFormat);

    textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    textFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    // 4. Рисуем текст белым цветом
    ID2D1SolidColorBrush * textBrush { nullptr };
    d2dContext->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Black), &textBrush);

    D2D1_RECT_F layoutRect = D2D1::RectF(0, 0, size.Width, size.Height);
    d2dContext->DrawText(text, static_cast<UINT32>(wcslen(text)), textFormat, &layoutRect, textBrush);

    // 5. Завершаем отрисовку и освобождаем D2D-ресурсы
    surfaceInterop->EndDraw();

    if(textBrush) textBrush->Release();
    if(textFormat) textFormat->Release();
    if(d2dContext) d2dContext->Release();

    // 6. Оборачиваем готовую поверхность в кисть Композитора
    auto surfaceBrush = g_compositor.CreateSurfaceBrush(surface);
    return surfaceBrush;
}

void CreateInterface() {
    if(!g_target) return;

    g_rootVisual = g_compositor.CreateContainerVisual();
    g_target.Root(g_rootVisual);

    // 1. Темно-серый фон окна
    g_backgroundVisual = g_compositor.CreateSpriteVisual();
    g_backgroundVisual.Brush(g_compositor.CreateColorBrush(winrt::Windows::UI::Color { 255, 255, 255, 255 }));
    g_rootVisual.Children().InsertAtTop(g_backgroundVisual);

    // 2. ОБЩИЙ КОНТЕЙНЕР ДЛЯ КНОПКИ
    g_buttonContainer = g_compositor.CreateContainerVisual();
    g_buttonContainer.Size(buttonSize);
    g_buttonContainer.AnchorPoint(float2 { 0.f, 0.f });
    //g_buttonContainer.RelativeOffsetAdjustment(float3 { 0.5f, 0.5f, 0.0f });
    //g_buttonContainer.Offset(float3 { 100.0f, 150.0f, 0.0f });

    // 3. СОЗДАЕМ СИНЮЮ ПОДЛОЖКУ КНОПКИ СО СКРУГЛЕНИЕМ 6px
    // 1. Геометрия ДЛЯ ЗАЛИВКИ (чуть меньше, чтобы углы не наслаивались)
    auto fillGeometry = g_compositor.CreateRoundedRectangleGeometry();
    fillGeometry.Size(buttonSize - Size { 2.0f, 2.0f });
    fillGeometry.CornerRadius(float2 { 7.0f, 7.0f }); // Радиус чуть меньше!

    g_buttonShape = g_compositor.CreateSpriteShape(fillGeometry);
    g_buttonShape.Offset(float2 { 1.0f, 1.0f }); // Центрируем внутренность
    g_buttonShape.FillBrush(g_compositor.CreateColorBrush(COLOR_NORMAL));

    // 2. Геометрия ДЛЯ КОНТУРА (строго по границам)
    auto strokeGeometry = g_compositor.CreateRoundedRectangleGeometry();
    strokeGeometry.Size(buttonSize);
    strokeGeometry.CornerRadius(float2 { 10.5f, 10.5f }); // Исходный радиус

    auto strokeShape = g_compositor.CreateSpriteShape(strokeGeometry);
    strokeShape.FillBrush(nullptr); // Прозрачная внутри
    strokeShape.StrokeBrush(g_compositor.CreateColorBrush(BORDER_NORMAL));
    strokeShape.StrokeThickness(2.f); // Чуть тоньше линию для аккуратности
    strokeShape.StrokeLineJoin(CompositionStrokeLineJoin::Round);

    g_buttonBg = g_compositor.CreateShapeVisual();
    g_buttonBg.Size(buttonSize);
    g_buttonBg.Shapes().Append(strokeShape);
    g_buttonBg.Shapes().Append(g_buttonShape);

    // Создаем связь: брать размер (Size) у Visual-контейнера, в котором лежит фигура
    auto geometrySizeExpression = g_compositor.CreateExpressionAnimation(L"Visual.Size");
    geometrySizeExpression.SetReferenceParameter(L"Visual", g_buttonBg);

    // 2. Привязываем выражение НАПРЯМУЮ К ГЕОМЕТРИИ
    //roundRectGeometry.StartAnimation(L"Size", geometrySizeExpression);

    // =========================================================================
    // ИСПРАВЛЕНИЕ: СОЗДАЕМ ИДЕАЛЬНУЮ СКРУГЛЕННУЮ ТЕНЬ
    // =========================================================================
    // Мы создаем визуальную поверхность из нашей скругленной кнопки
    auto visualSurface = g_compositor.CreateVisualSurface();
    visualSurface.SourceVisual(g_buttonBg);
    visualSurface.SourceSize(buttonSize);

    // Оборачиваем эту скругленную поверхность в кисть-маску
    auto maskBrush = g_compositor.CreateSurfaceBrush(visualSurface);

    g_buttonShadow = g_compositor.CreateDropShadow();
    g_buttonShadow.Color(winrt::Windows::UI::Color { 255, 0, 0, 0 });
    g_buttonShadow.Opacity(0.4f);
    g_buttonShadow.BlurRadius(6.0f);
    g_buttonShadow.Offset(float3 { 2.0f, 2.0f, 0.0f });

    // ВАЖНО: Передаем скругленную маску в тень! Теперь тень тоже скруглена на 6px.
    g_buttonShadow.Mask(maskBrush);

    g_shadowVisual = g_compositor.CreateSpriteVisual();
    g_shadowVisual.Size(buttonSize);
    g_shadowVisual.Shadow(g_buttonShadow);
    //g_shadowVisual.IsVisible(false);

    // Кладем тень на самый нижний слой внутри контейнера
    g_buttonContainer.Children().InsertAtTop(g_shadowVisual);
    // =========================================================================

    // 4. Кладем синюю подложку поверх тени
    g_buttonContainer.Children().InsertAtTop(g_buttonBg);

    // 5. СОЗДАЕМ ТЕКСТ КНОПКИ
    g_textVisual = g_compositor.CreateSpriteVisual();
    g_textVisual.Size(buttonSize - Size {2.0f, 2.0f});
    g_textVisual.Brush(CreateTextBrush(L"Нажми меня", buttonSize));

    g_buttonContainer.Children().InsertAtTop(g_textVisual);
    g_backgroundVisual.Children().InsertAtTop(g_buttonContainer);
}

void AnimateButtonPress(bool isPressed) {
    if(!g_buttonContainer || !g_buttonShadow || !g_shadowVisual) return;

    // Сдвигаем ВЕСЬ контейнер (кнопка + текст + тень) вперед на 2 пикселя
    float3 targetContainerOffset = isPressed ? float3 { 1.0f, 1.0f, 0.0f } + buttonPos : buttonPos;
    Size targetContainerSize = isPressed ? buttonSize - Size { 1.0f, 1.0f} : buttonSize;
    // Локально компенсируем тень, сдвигая её визуал обратно, чтобы она казалась прижатой
    float3 targetShadowOffset = isPressed ? float3 { -1.0f, -1.0f, 0.0f } : float3 { 0.0f, 0.0f, 0.0f };

    float targetBlur = isPressed ? 3.0f : 6.0f;

    auto easeFunction = g_compositor.CreateCubicBezierEasingFunction({ 0.25f, 0.1f }, { 0.25f, 1.0f });
    auto duration = std::chrono::milliseconds(80);

    // 1. Анимация контейнера
    auto containerAnim1 = g_compositor.CreateVector3KeyFrameAnimation();
    containerAnim1.Duration(duration);
    containerAnim1.InsertKeyFrame(1.0f, targetContainerOffset, easeFunction);
    g_buttonContainer.StartAnimation(L"Offset", containerAnim1);

    // Анимация изменения размера
    auto containerAnim2 = g_compositor.CreateVector2KeyFrameAnimation();
    containerAnim2.Duration(duration);
    containerAnim2.InsertKeyFrame(1.0f, targetContainerSize, easeFunction);
    g_buttonBg.StartAnimation(L"Size", containerAnim2);
    g_buttonContainer.StartAnimation(L"Size", containerAnim2);


    // 2. Анимация компенсации визуала тени
    auto shadowVisualAnim = g_compositor.CreateVector3KeyFrameAnimation();
    shadowVisualAnim.Duration(duration);
    shadowVisualAnim.InsertKeyFrame(1.0f, targetShadowOffset, easeFunction);
    g_shadowVisual.StartAnimation(L"Offset", shadowVisualAnim);

    // 3. Анимация размытия тени
    auto blurAnim = g_compositor.CreateScalarKeyFrameAnimation();
    blurAnim.Duration(duration);
    blurAnim.InsertKeyFrame(1.0f, targetBlur, easeFunction);
    g_buttonShadow.StartAnimation(L"BlurRadius", blurAnim);


    //// 4. Анимация прозрачности тени
    //auto opacityAnim = g_compositor.CreateScalarKeyFrameAnimation();
    //opacityAnim.Duration(duration);
    //opacityAnim.InsertKeyFrame(1.0f, targetOpacity, easeFunction);
    //g_buttonShadow.StartAnimation(L"Opacity", opacityAnim);
}

void AnimateButtonColor(winrt::Windows::UI::Color targetColor) {
    if(!g_buttonShape) return;

    // Получаем brush, который раскрашивает нашу фигуру
    auto brush = g_buttonShape.FillBrush().as<CompositionColorBrush>();

    // Создаем анимацию ключевых кадров для цвета
    auto colorAnimation = g_compositor.CreateColorKeyFrameAnimation();
    colorAnimation.Duration(std::chrono::milliseconds(500)); // Длительность перехода 200 мс

    // Используем плавную кривую Безье (разгон-замедление) вместо линейной функции
    auto easeFunction = g_compositor.CreateCubicBezierEasingFunction({ 0.25f, 0.1f }, { 0.25f, 1.0f });

    // Конечная точка анимации (1.0f означает 100% времени анимации)
    colorAnimation.InsertKeyFrame(1.0f, targetColor, easeFunction);

    // Запускаем анимацию свойства "Color" у кисти
    brush.StartAnimation(L"Color", colorAnimation);
}

void UpdateInterfaceLayout(float windowWidth, float windowHeight) {
    if(windowWidth <= 0.0f || windowHeight <= 0.0f) return;

    if(g_backgroundVisual) {
        g_backgroundVisual.Size(float2 { windowWidth, windowHeight });
    }
    // Сам g_buttonContainer центрируется автоматически через RelativeOffsetAdjustment
    if(g_buttonContainer) {
        buttonPos.X = (windowWidth - buttonSize.Width) / 2.0f;
        buttonPos.Y = (windowHeight - buttonSize.Height) / 2.0f;

        g_buttonContainer.Offset(buttonPos);
    }
}


int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    winrt::init_apartment(winrt::apartment_type::single_threaded);

    WNDCLASSEX wcex = { sizeof(WNDCLASSEX) };
    wcex.style = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc = WndProc;
    wcex.hInstance = hInstance;
    wcex.lpszClassName = L"WinRtDesktopClass";
    wcex.hCursor = LoadCursor(NULL, IDC_ARROW);

    // ВАЖНО 1: Убираем GDI кисть. Теперь Win32 не будет красить окно в белый цвет.
    wcex.hbrBackground = NULL;

    RegisterClassEx(&wcex);

    HWND hWnd = CreateWindowEx(
        WS_EX_NOREDIRECTIONBITMAP,
        L"WinRtDesktopClass", L"WinRT Composition",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 800, 600, NULL, NULL, hInstance, NULL);

    if(!hWnd) return FALSE;

    InitWinRTComposition(hWnd);
    CreateInterface();

    // Получаем реальный размер клиентской области окна перед первым показом
    RECT rc;
    GetClientRect(hWnd, &rc);
    UpdateInterfaceLayout(static_cast<float>(rc.right), static_cast<float>(rc.bottom));

    ShowWindow(hWnd, nCmdShow);
    UpdateWindow(hWnd);

    MSG msg;
    while(GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return (int)msg.wParam;
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch(message) {
    case WM_ERASEBKGND:
        return 1;

    case WM_LBUTTONDOWN:
    {
        if(g_isMouseOver) {
            AnimateButtonPress(true); // Кнопка нажата (сдвигается вниз, тень уменьшается)
            SetCapture(hWnd);
        }

        break;
    }

    case WM_LBUTTONUP:
    {
        // Освобождаем захват мыши
        ReleaseCapture();

        // Проверяем, где отпустили мышь
        float mouseX = static_cast<float>(LOWORD(lParam));
        float mouseY = static_cast<float>(HIWORD(lParam));

        RECT rc;
        GetClientRect(hWnd, &rc);
        float btnLeft = (static_cast<float>(rc.right) - buttonSize.Width) / 2.0f;
        float btnTop = (static_cast<float>(rc.bottom) - buttonSize.Height) / 2.0f;

        bool isInside = (mouseX >= btnLeft && mouseX <= btnLeft + buttonSize.Width &&
            mouseY >= btnTop && mouseY <= btnTop + buttonSize.Height);

        AnimateButtonPress(false);

        if(isInside) {
            //essageBox(hWnd, L"Кнопка успешно нажата!", L"WinRT", MB_OK | MB_ICONINFORMATION);
        }
        break;
    }

    case WM_MOUSEMOVE:
    {
        float mouseX = static_cast<float>(LOWORD(lParam));
        float mouseY = static_cast<float>(HIWORD(lParam));

        RECT rc;
        GetClientRect(hWnd, &rc);
        float btnLeft = (static_cast<float>(rc.right) - buttonSize.Width) / 2.0f;
        float btnTop = (static_cast<float>(rc.bottom) - buttonSize.Height) / 2.0f;

        bool isInside = (mouseX >= btnLeft && mouseX <= btnLeft + buttonSize.Width &&
            mouseY >= btnTop && mouseY <= btnTop + buttonSize.Height);

        if(isInside && !g_isMouseOver) {
            g_isMouseOver = true;
            AnimateButtonColor(COLOR_HOVER);

            TRACKMOUSEEVENT tme = { sizeof(TRACKMOUSEEVENT), TME_LEAVE, hWnd, HOVER_DEFAULT };
            TrackMouseEvent(&tme);
        }
        else if(!isInside && g_isMouseOver) {
            g_isMouseOver = false;
            AnimateButtonColor(COLOR_NORMAL);
        }
        break;
    }

    case WM_MOUSELEAVE:
        if(g_isMouseOver) {
            g_isMouseOver = false;
            AnimateButtonColor(COLOR_NORMAL);
            AnimateButtonPress(false); // Сброс состояния, если мышь увели во время зажатия
        }
        break;

    case WM_SIZE:
        if(g_target) {
            UpdateInterfaceLayout(static_cast<float>(LOWORD(lParam)), static_cast<float>(HIWORD(lParam)));
        }
        break;

    case WM_DESTROY:
        PostQuitMessage(0);
        break;
    default:
        return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

