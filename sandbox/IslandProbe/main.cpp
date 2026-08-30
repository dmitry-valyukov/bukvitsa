// Проба архитектуры «своё окно + остров-карточка».
//
// Окно верхнего уровня создаётся как в sandbox/WindowsCompositor: без кисти
// фона, без поверхности перенаправления (WS_EX_NOREDIRECTIONBITMAP), картинка
// заставки — визуалом композитора, и на WM_SIZE меняется одно его свойство.
// Поверх окна — остров WinUI (DesktopWindowXamlSource), но не во всё окно, а
// прямоугольником с карточку кнопок; его мост двигается тем же WM_SIZE.
//
// Проба отвечает на два вопроса:
//  1) прозрачен ли грунт острова — по умолчанию и с прозрачным SystemBackdrop
//     (клавиша B переключает; текущее состояние написано в заголовке окна);
//  2) успевает ли картинка и карточка за рамкой при быстрой растяжке, когда
//     остров маленький и перевёрстывать ему нечего.
#include <windows.h>

#include <DispatcherQueue.h>
#include <MddBootstrap.h>
#include <d2d1_1.h>
#include <d3d11.h>
#include <unknwn.h>
#include <wincodec.h>
#include <windows.ui.composition.interop.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Composition.Desktop.h>
#include <winrt/Windows.UI.Composition.h>

// GetCurrentTime из windows.h — макрос, который иначе подставился бы прямо в
// объявление Storyboard::GetCurrentTime; тот же приём, что в pch песочницы.
#undef GetCurrentTime
#include <winrt/Microsoft.UI.Composition.h>
#include <winrt/Microsoft.UI.Content.h>
#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Hosting.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.h>

#pragma comment(lib, "windowsapp.lib")
#pragma comment(lib, "coremessaging.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "windowscodecs.lib")

namespace {

namespace wuc = winrt::Windows::UI::Composition;
namespace xaml = winrt::Microsoft::UI::Xaml;

// Заставка Буквицы; нет файла — серый фон, проба не про раскодирование.
constexpr const wchar_t* kSplash =
    L"M:\\worktrees\\window-backdrop\\Reader\\src\\Assets\\splash-screen-1k.png";

// Прямоугольник карточки: прижат к правому верху, как на стартовом экране.
constexpr int kCardWidth = 360;
constexpr int kCardHeight = 320;
constexpr int kCardMarginRight = 72;
constexpr int kCardMarginTop = 64;

winrt::Windows::System::DispatcherQueueController g_systemQueue{nullptr};
winrt::Microsoft::UI::Dispatching::DispatcherQueueController g_uiQueue{nullptr};
xaml::Hosting::WindowsXamlManager g_xaml{nullptr};

wuc::Compositor g_compositor{nullptr};
wuc::Desktop::DesktopWindowTarget g_target{nullptr};
wuc::SpriteVisual g_background{nullptr};
wuc::CompositionGraphicsDevice g_graphics{nullptr};

xaml::Hosting::DesktopWindowXamlSource g_island{nullptr};
bool g_clearBackdrop = false;

/// Прозрачный задник: его дело не рисовать, а самим фактом подключения
/// перевести корень острова из непрозрачного грунта в прозрачный.
struct ClearBackdrop : xaml::Media::SystemBackdropT<ClearBackdrop> {
    void OnTargetConnected(
        winrt::Microsoft::UI::Composition::ICompositionSupportsSystemBackdrop const& target,
        xaml::XamlRoot const&) {
        target.SystemBackdrop(g_compositor.CreateColorBrush({0, 0, 0, 0}));
    }
    void OnTargetDisconnected(
        winrt::Microsoft::UI::Composition::ICompositionSupportsSystemBackdrop const& target) {
        target.SystemBackdrop(nullptr);
    }
};

void ensureSystemQueue() {
    if (winrt::Windows::System::DispatcherQueue::GetForCurrentThread()) return;
    DispatcherQueueOptions const options{sizeof(DispatcherQueueOptions), DQTYPE_THREAD_CURRENT,
                                         DQTAT_COM_STA};
    ABI::Windows::System::IDispatcherQueueController* raw = nullptr;
    winrt::check_hresult(::CreateDispatcherQueueController(options, &raw));
    winrt::copy_from_abi(g_systemQueue, raw);
    raw->Release();
}

wuc::CompositionGraphicsDevice graphicsDevice() {
    winrt::com_ptr<ID3D11Device> d3d;
    winrt::check_hresult(::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                             D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                             D3D11_SDK_VERSION, d3d.put(), nullptr, nullptr));
    D2D1_CREATION_PROPERTIES const properties{D2D1_THREADING_MODE_SINGLE_THREADED,
                                              D2D1_DEBUG_LEVEL_NONE,
                                              D2D1_DEVICE_CONTEXT_OPTIONS_NONE};
    winrt::com_ptr<ID2D1Device> d2d;
    winrt::check_hresult(::D2D1CreateDevice(d3d.as<IDXGIDevice>().get(), &properties, d2d.put()));

    auto const interop = g_compositor.as<ABI::Windows::UI::Composition::ICompositorInterop>();
    winrt::com_ptr<ABI::Windows::UI::Composition::ICompositionGraphicsDevice> raw;
    winrt::check_hresult(interop->CreateGraphicsDevice(d2d.as<::IUnknown>().get(), raw.put()));
    return raw.as<wuc::CompositionGraphicsDevice>();
}

/// Картинка — в кисть композитора: поверхность в натуральную величину снимка,
/// растяжка UniformToFill к верхней середине — дело самой кисти.
wuc::CompositionBrush splashBrush() {
    winrt::com_ptr<IWICImagingFactory> wic;
    winrt::check_hresult(::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                            IID_PPV_ARGS(wic.put())));
    winrt::com_ptr<IWICBitmapDecoder> decoder;
    if (FAILED(wic->CreateDecoderFromFilename(kSplash, nullptr, GENERIC_READ,
                                              WICDecodeMetadataCacheOnLoad, decoder.put()))) {
        return g_compositor.CreateColorBrush({255, 96, 96, 96});
    }
    winrt::com_ptr<IWICBitmapFrameDecode> frame;
    winrt::check_hresult(decoder->GetFrame(0, frame.put()));
    winrt::com_ptr<IWICFormatConverter> converter;
    winrt::check_hresult(wic->CreateFormatConverter(converter.put()));
    winrt::check_hresult(converter->Initialize(frame.get(), GUID_WICPixelFormat32bppPBGRA,
                                               WICBitmapDitherTypeNone, nullptr, 0.0,
                                               WICBitmapPaletteTypeMedianCut));
    UINT width = 0;
    UINT height = 0;
    winrt::check_hresult(converter->GetSize(&width, &height));

    g_graphics = graphicsDevice();
    wuc::CompositionDrawingSurface const surface = g_graphics.CreateDrawingSurface(
        {static_cast<float>(width), static_cast<float>(height)},
        winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
        winrt::Windows::Graphics::DirectX::DirectXAlphaMode::Premultiplied);

    auto const interop =
        surface.as<ABI::Windows::UI::Composition::ICompositionDrawingSurfaceInterop>();
    winrt::com_ptr<ID2D1DeviceContext> context;
    POINT offset{};
    winrt::check_hresult(interop->BeginDraw(nullptr, __uuidof(ID2D1DeviceContext),
                                            context.put_void(), &offset));
    context->SetTransform(D2D1::Matrix3x2F::Translation(static_cast<float>(offset.x),
                                                        static_cast<float>(offset.y)));
    winrt::com_ptr<ID2D1Bitmap1> bitmap;
    if (SUCCEEDED(context->CreateBitmapFromWicBitmap(converter.get(), nullptr, bitmap.put()))) {
        context->DrawBitmap(bitmap.get(),
                            D2D1::RectF(0.0f, 0.0f, static_cast<float>(width),
                                        static_cast<float>(height)),
                            1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    }
    winrt::check_hresult(interop->EndDraw());

    wuc::CompositionSurfaceBrush const brush = g_compositor.CreateSurfaceBrush(surface);
    brush.Stretch(wuc::CompositionStretch::UniformToFill);
    brush.HorizontalAlignmentRatio(0.5f);
    brush.VerticalAlignmentRatio(0.0f);
    return brush;
}

/// Дерево карточки: полупрозрачная скруглённая подложка с полем вокруг — по
/// тому, что видно в поле и сквозь подложку, прозрачность грунта острова
/// читается с одного взгляда.
xaml::UIElement cardContent() {
    xaml::Controls::Button button;
    button.Content(winrt::box_value(L"Нажми меня"));
    button.HorizontalAlignment(xaml::HorizontalAlignment::Center);
    button.VerticalAlignment(xaml::VerticalAlignment::Center);

    xaml::Controls::Border card;
    card.CornerRadius({12, 12, 12, 12});
    card.Margin({24, 24, 24, 24});
    card.Padding({16, 16, 16, 16});
    card.Background(xaml::Media::SolidColorBrush{
        winrt::Windows::UI::Color{0x6C, 0x1C, 0x12, 0x08}});
    card.Child(button);
    return card;
}

RECT cardRect(int clientWidth) {
    int const left = clientWidth - kCardMarginRight - kCardWidth;
    return {left, kCardMarginTop, left + kCardWidth, kCardMarginTop + kCardHeight};
}

void applyTitle(HWND hwnd) {
    ::SetWindowTextW(hwnd, g_clearBackdrop
                               ? L"Проба острова — грунт: прозрачный SystemBackdrop (B)"
                               : L"Проба острова — грунт: по умолчанию (B)");
}

LRESULT CALLBACK wndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_ERASEBKGND:
            return 1;

        case WM_SIZE:
            if (wparam != SIZE_MINIMIZED) {
                if (g_background) {
                    g_background.Size({static_cast<float>(LOWORD(lparam)),
                                       static_cast<float>(HIWORD(lparam))});
                }
                if (g_island) {
                    RECT const card = cardRect(LOWORD(lparam));
                    g_island.SiteBridge().MoveAndResize(
                        {card.left, card.top, card.right - card.left, card.bottom - card.top});
                }
            }
            break;

        case WM_KEYDOWN:
            if (wparam == 'B' && g_island) {
                g_clearBackdrop = !g_clearBackdrop;
                g_island.SystemBackdrop(g_clearBackdrop ? winrt::make<ClearBackdrop>()
                                                        : xaml::Media::SystemBackdrop{nullptr});
                applyTitle(hwnd);
            }
            break;

        case WM_DESTROY:
            ::PostQuitMessage(0);
            break;
    }
    return ::DefWindowProcW(hwnd, message, wparam, lparam);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    // Загрузчик платформы — до первого типа WinAppSDK; та же строка, что в
    // wxl (impl/bootstrap.cpp): выпуск 2.x без плавающего минимума.
    winrt::check_hresult(::MddBootstrapInitialize(0x00020000, nullptr, {}));
    winrt::init_apartment(winrt::apartment_type::single_threaded);

    // Две очереди на одном потоке: своя у WinUI (без неё не живёт остров) и
    // Windows.System (без неё не создаётся системный композитор). Так же
    // сосуществуют они в приложениях с Mica.
    g_uiQueue =
        winrt::Microsoft::UI::Dispatching::DispatcherQueueController::CreateOnCurrentThread();
    g_xaml = xaml::Hosting::WindowsXamlManager::InitializeForCurrentThread();
    ensureSystemQueue();

    WNDCLASSEXW wc{sizeof(WNDCLASSEXW)};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = instance;
    wc.lpszClassName = L"BukvitsaIslandProbe";
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;  // никакой GDI-кисти — белому взяться неоткуда
    ::RegisterClassExW(&wc);

    HWND const hwnd = ::CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP, wc.lpszClassName, L"",
                                        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1280,
                                        860, nullptr, nullptr, instance, nullptr);
    if (!hwnd) return 1;
    applyTitle(hwnd);

    g_compositor = wuc::Compositor{};
    auto const interop =
        g_compositor.as<ABI::Windows::UI::Composition::Desktop::ICompositorDesktopInterop>();
    ABI::Windows::UI::Composition::Desktop::IDesktopWindowTarget* rawTarget = nullptr;
    winrt::check_hresult(interop->CreateDesktopWindowTarget(hwnd, false, &rawTarget));
    g_target = {rawTarget, winrt::take_ownership_from_abi};

    g_background = g_compositor.CreateSpriteVisual();
    g_background.Brush(splashBrush());
    RECT client{};
    ::GetClientRect(hwnd, &client);
    g_background.Size({static_cast<float>(client.right), static_cast<float>(client.bottom)});
    g_target.Root(g_background);

    g_island = xaml::Hosting::DesktopWindowXamlSource{};
    g_island.Initialize({reinterpret_cast<uint64_t>(hwnd)});
    g_island.Content(cardContent());
    RECT const card = cardRect(client.right);
    g_island.SiteBridge().MoveAndResize(
        {card.left, card.top, card.right - card.left, card.bottom - card.top});
    g_island.SiteBridge().Show();

    ::ShowWindow(hwnd, show);
    ::UpdateWindow(hwnd);

    MSG msg;
    while (::GetMessageW(&msg, nullptr, 0, 0)) {
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}
