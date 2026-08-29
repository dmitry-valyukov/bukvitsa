#include <algorithm>
#include <cmath>
#include <vector>

#include <windows.h>

#include <d2d1_1.h>
#include <wincodec.h>

// Заголовки проекта после стандартных. Свой первым.
#include "skin_wizard.h"

#include "card.h"
#include "imaging.h"

namespace bukvitsa::reader {

using namespace wxl;

namespace {

// Обстановка мастера — те же цвета, что у панели читалки: диалог имени и
// кнопки не бумага, а инструмент.
constexpr uint32_t kChrome = 0xF21E1E22;
constexpr uint32_t kInk = 0xFFE8E4DC;
constexpr uint32_t kEdge = 0x33FFFFFF;

// Кнопки — как на стартовом экране: та же ширина, та же полупрозрачность,
// под ними должна просвечивать страница; где им стоять, знает карточка
// (card.h). Отмена — чуть серее остальных, она уводит, а не ведёт.
constexpr float kButtonWidth = 300.0f;
constexpr float kRestingOpacity = 0.92f;
constexpr uint32_t kCancelFace = 0xFFD9D6D2;

// Сетка поверх страницы — подсказка, а не занавес: все линии сильно
// полупрозрачны, центральная ярче тоном, чтобы читаться сквозь текст.
// Кружочки полупрозрачны, как кнопки: под ними тоже страница.
constexpr D2D1_COLOR_F kCurveColor{1.0f, 0.85f, 0.45f, 0.6f};
constexpr D2D1_COLOR_F kGripFill{1.0f, 1.0f, 1.0f, 0.55f};
constexpr D2D1_COLOR_F kGripRing{0.15f, 0.12f, 0.08f, 0.7f};

/// Тень кривой: две чёрно-коричневые полупрозрачные линии в пиксель над и
/// под основной — они оттеняют её на светлой бумаге.
constexpr D2D1_COLOR_F kCurveShade{0.11f, 0.07f, 0.03f, 0.3f};

constexpr float kGripRadius = 7.0f;   ///< рисуемый кружочек, DIP
constexpr float kGripReach = 12.0f;   ///< зона захвата: шире кружочка, промах злит
constexpr float kCurveStep = 4.0f;    ///< шаг ломаной, которой рисуется кривая

/// Ближе этого точкам одной кривой не сойтись: кривой нужен ход X между
/// соседями, иначе сегмент вырождается.
constexpr float kMinGap = 0.02f;

/// Сколько линий-подсказок между верхней и нижней кривыми листа, считая их
/// самих.
constexpr int kGuideRows = 9;

/// Пусто ли имя — пробелы не в счёт.
bool blank(std::wstring_view text) {
    return std::all_of(text.begin(), text.end(),
                       [](wchar_t c) { return c == L' ' || c == L'\t'; });
}

}  // namespace

SkinWizard::SkinWizard(const Compositor& compositor) : compositor_(compositor) {
    buildTree();
}

EdgeCurve& SkinWizard::curve(int index) {
    switch (index) {
        case 0: return skin_.topLeft;
        case 1: return skin_.topRight;
        case 2: return skin_.bottomLeft;
        default: return skin_.bottomRight;
    }
}

const EdgeCurve& SkinWizard::curve(int index) const {
    return const_cast<SkinWizard*>(this)->curve(index);
}

Button SkinWizard::overlayButton(std::wstring_view caption, float tall, float kegel, bool cancel,
                                 void (SkinWizard::*handler)()) {
    using namespace wxl::dsl;

    auto button = Button{
        caption,
        width = kButtonWidth,
        height = tall,
        FontWeight{600},
        Margin{0, 6},
        fontSize = kegel,
        onClick = [this, handler](Object const&, RoutedEventArgs&) { (this->*handler)(); },
    };

    if (cancel) button.background(SolidColorBrush{ARGB{kCancelFace}});

    // Полупрозрачность — визуалом, как у стартового экрана, только без
    // анимации появления: мастер открывают действием, ждать ему нечего.
    ElementCompositionPreview::getElementVisual(button).opacity(kRestingOpacity);
    return button;
}

void SkinWizard::buildTree() {
    using namespace wxl::dsl;

    surfaceHost_ = Grid{};

    // Кнопки — на той же карточке и на том же месте, что у стартового
    // экрана: «Сохранить» увеличена, как «Продолжить чтение», — это действие
    // по умолчанию, его же зовёт Enter; «Выйти из мастера обложек» — отмена,
    // её зовёт Escape.
    auto buttons = buttonCard(StackPanel{
        overlayButton(L"Сохранить", 72.0f, 19.0f, false, &SkinWizard::saveRequested),
        overlayButton(L"Выбрать другое изображение", 46.0f, 15.0f, false,
                      &SkinWizard::chooseAnother),
        overlayButton(L"Выйти из мастера обложек", 46.0f, 15.0f, true,
                      &SkinWizard::exitWizard),
    });

    nameBox_ = TextBox{width = 320.0};

    namePanel_ = Border{
        hAlign.center,
        vAlign.center,
        visibility = Visibility::Collapsed,
        background = SolidColorBrush{ARGB{kChrome}},
        borderBrush = SolidColorBrush{ARGB{kEdge}},
        BorderThickness{1},
        CornerRadius{6},
        Padding{20, 16},
        StackPanel{
            TextBlock{
                L"Название обложки",
                fontSize = 13,
                foreground = SolidColorBrush{ARGB{kInk}},
                Margin{0, 0, 0, 8},
            },
            nameBox_.value(),
            StackPanel{
                Orientation::Horizontal,
                hAlign.right,
                Margin{0, 12, 0, 0},
                Button{
                    L"ОК",
                    Padding{18, 6},
                    Margin{0, 0, 8, 0},
                    onClick = [this](Object const&, RoutedEventArgs&) { finishNaming(true); },
                },
                Button{
                    L"Отмена",
                    Padding{18, 6},
                    onClick = [this](Object const&, RoutedEventArgs&) { finishNaming(false); },
                },
            },
        },
    };

    auto tree = Grid{
        isTabStop = true,
        visibility = Visibility::Collapsed,
        // Прозрачная, но настоящая кисть: без неё оверлей не участвует в
        // проверке попадания, и тянуть точки было бы не за что.
        background = SolidColorBrush{ARGB{0x00000000}},
        surfaceHost_.value(),
        buttons,
        namePanel_.value(),
    };

    // Enter — действие по умолчанию: «Сохранить», а в открытом диалоге имени
    // — его «ОК». Escape — отмена: «Выйти из мастера обложек», а в диалоге —
    // его «Отмена». На пути вниз, чтобы клавиши работали при любом фокусе.
    tree.add_onPreviewKeyDown([this](Object const&, KeyRoutedEventArgs& args) {
        const bool naming = namePanel_.value().visibility() == Visibility::Visible;
        switch (args.key()) {
            case VirtualKey::Enter:
                if (naming) {
                    finishNaming(true);
                } else {
                    saveRequested();
                }
                break;
            case VirtualKey::Escape:
                if (naming) {
                    finishNaming(false);
                } else {
                    exitWizard();
                }
                break;
            default: return;
        }
        args.handled(true);
    });

    tree.add_onSizeChanged([this](Object const&, SizeChangedEventArgs&) {
        // Координаты в долях, поэтому смена размеров ничего не двигает по
        // существу — точки остаются на своих местах снимка.
        if (resizeSurface()) redraw();
    });

    tree.add_onPointerPressed([this](Object const&, PointerRoutedEventArgs& args) {
        // Фокус — себе на каждом нажатии: щелчок по книге уводил его с
        // мастера, и Enter с Escape переставали работать.
        root_.value().focus(FocusState::Programmatic);

        const PointerPoint touch = args.getCurrentPoint(root_.value());
        if (!touch.properties().isLeftButtonPressed()) return;

        int curveIndex = 0;
        std::size_t pointIndex = 0;
        if (!gripAt(touch.position(), curveIndex, pointIndex)) return;

        dragging_ = true;
        dragCurve_ = curveIndex;
        dragPoint_ = pointIndex;
        args.handled(true);
    });

    tree.add_onPointerMoved([this](Object const&, PointerRoutedEventArgs& args) {
        const Point point = args.getCurrentPoint(root_.value()).position();

        bool overGrip = dragging_;
        if (dragging_) {
            EdgeCurve& edited = curve(dragCurve_);
            const std::size_t at = dragPoint_;
            constexpr std::size_t last = static_cast<std::size_t>(EdgeCurve::kPoints) - 1;

            // Точка ходит в обе оси. По вертикали — от кромки до четверти
            // высоты; по горизонтали — между соседками, не выходя со своей
            // половины разворота: середина — граница листов.
            // Не top/left: это имена тегов DSL, и локальная переменная их
            // прятала бы.
            const bool onTop = dragCurve_ < 2;
            const bool onLeft = dragCurve_ % 2 == 0;

            edited.y[at] = onTop ? std::clamp(point.y / height_, 0.0f, kEdgeReach)
                                 : std::clamp(point.y / height_, 1.0f - kEdgeReach, 1.0f);

            const float low = at == 0 ? (onLeft ? 0.0f : 0.5f) : edited.x[at - 1] + kMinGap;
            const float high = at == last ? (onLeft ? 0.5f : 1.0f) : edited.x[at + 1] - kMinGap;
            edited.x[at] = std::clamp(point.x / width_, low, high);

            redraw();
            args.handled(true);
        } else {
            int curveIndex = 0;
            std::size_t pointIndex = 0;
            overGrip = gripAt(point, curveIndex, pointIndex);
        }

        // Курсор — каждое движение заново: WinUI возвращает свою стрелку, а
        // задать курсор элементу проекция не умеет. Макрос ресурса Windows
        // допустим здесь — спрашиваем саму Windows. Все четыре стрелки:
        // точка ходит в обе оси.
        if (overGrip) ::SetCursor(::LoadCursorW(nullptr, IDC_SIZEALL));
    });

    tree.add_onPointerReleased([this](Object const&, PointerRoutedEventArgs& args) {
        if (!dragging_) return;
        dragging_ = false;
        args.handled(true);

        // Точку отпустили — кривые устоялись: время пересчитать карту изгиба
        // и показать страницу по-новому. Не на каждом движении: пересборка
        // карты стоит прохода по всем пикселям слоя.
        if (onCurvesChanged) onCurvesChanged();
    });

    root_ = tree;
}

bool SkinWizard::openNew(std::filesystem::path image) {
    // Проверка снимка — здесь, чтобы не входить в мастер с пустой подложкой:
    // рисовать его будет полоса, но отказ она глотает молча.
    if (!decodeImage(image)) return false;

    image_ = std::move(image);
    skin_ = defaultSkin();
    dragging_ = false;
    namePanel_.value().visibility(Visibility::Collapsed);

    redraw();
    return true;
}

bool SkinWizard::openEdit(const Skin& skin) {
    std::filesystem::path image = skinDirectory() / skin.image;
    if (!decodeImage(image)) return false;

    image_ = std::move(image);
    skin_ = skin;
    dragging_ = false;
    namePanel_.value().visibility(Visibility::Collapsed);

    redraw();
    return true;
}

void SkinWizard::show() {
    if (open_) return;
    open_ = true;
    root_.value().visibility(Visibility::Visible);
    root_.value().focus(FocusState::Programmatic);
}

void SkinWizard::hide() {
    if (!open_) return;
    open_ = false;
    dragging_ = false;
    namePanel_.value().visibility(Visibility::Collapsed);
    root_.value().visibility(Visibility::Collapsed);
}

bool SkinWizard::resizeSurface() {
    const auto width = static_cast<float>(root_.value().actualWidth());
    const auto height = static_cast<float>(root_.value().actualHeight());

    Nullable<XamlRoot> const xamlRoot = root_.value().xamlRoot();
    float scale = xamlRoot ? static_cast<float>(xamlRoot->rasterizationScale()) : 1.0f;
    if (scale <= 0.0f) scale = 1.0f;

    if (width == width_ && height == height_ && scale == scale_ && !surface_.empty()) return false;

    width_ = width;
    height_ = height;
    scale_ = scale;

    const SizeInt32 pixels{static_cast<int32_t>(width * scale + 0.5f),
                           static_cast<int32_t>(height * scale + 0.5f)};
    if (pixels.width <= 0 || pixels.height <= 0) return false;

    if (surface_.empty()) {
        surface_.emplace_back(compositor_, pixels);
        SpriteVisual visual = compositor_.createSpriteVisual();
        visual.brush(surface_[0].brush());
        ElementCompositionPreview::setElementChildVisual(surfaceHost_.value(), visual);
        visual_ = visual;
    } else {
        surface_[0].resize(pixels);
    }

    visual_.value().size({width, height});
    return true;
}

void SkinWizard::redraw() {
    if (surface_.empty() || width_ <= 0.0f || height_ <= 0.0f) return;

    surface_[0].draw([this](ID2D1DeviceContext* context) {
        // Поверхность в пикселях, рисование в DIP — как у полосы набора.
        D2D1_MATRIX_3X2_F atlas{};
        context->GetTransform(&atlas);
        context->SetTransform(D2D1::Matrix3x2F::Scale(scale_, scale_) *
                              *D2D1::Matrix3x2F::ReinterpretBaseType(&atlas));
        context->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

        // Прозрачный лист: снимок и страницу рисует полоса под оверлеем.
        context->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> curveBrush;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> shade;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> fill;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> ring;
        context->CreateSolidColorBrush(kCurveColor, &curveBrush);
        context->CreateSolidColorBrush(kCurveShade, &shade);
        context->CreateSolidColorBrush(kGripFill, &fill);
        context->CreateSolidColorBrush(kGripRing, &ring);
        if (!curveBrush || !shade || !fill || !ring) return;

        // Смещение теневых линий — ровно пиксель экрана, какой бы ни был
        // масштаб: рисуем в DIP, а пиксель хотим физический.
        const float pixel = 1.0f / scale_;

        // Линии-подсказки: у каждого листа свои, от его верхней кривой к его
        // нижней, и только между крайними точками — за ними кривая всё равно
        // держит их значение, и линия во всю ширину лишь мешала бы снимку.
        for (int half = 0; half < 2; ++half) {
            // Не top/bottom: это имена тегов DSL.
            const EdgeCurve& upper = curve(half);
            const EdgeCurve& lower = curve(half + 2);
            constexpr std::size_t last = static_cast<std::size_t>(EdgeCurve::kPoints) - 1;

            for (int row = 0; row < kGuideRows; ++row) {
                const float share = static_cast<float>(row) / (kGuideRows - 1);
                const float base = kEdgeInset + share * (1.0f - 2.0f * kEdgeInset);
                const float from = upper.x[0] + (lower.x[0] - upper.x[0]) * share;
                const float to = upper.x[last] + (lower.x[last] - upper.x[last]) * share;

                const int steps =
                    std::max(2, static_cast<int>((to - from) * width_ / kCurveStep));
                D2D1_POINT_2F previous{};

                for (int step = 0; step <= steps; ++step) {
                    const float u =
                        from + (to - from) * static_cast<float>(step) / static_cast<float>(steps);
                    const float deviation = (edgeAt(upper, u) - kEdgeInset) * (1.0f - share) +
                                            (edgeAt(lower, u) - (1.0f - kEdgeInset)) * share;
                    const D2D1_POINT_2F point{u * width_, (base + deviation) * height_};

                    // Линия рисуется тройкой: тёмная в пиксель выше, тёмная в
                    // пиксель ниже и основная поверх — тень отбивает её и от
                    // светлой бумаги, и от текста.
                    if (step > 0) {
                        context->DrawLine({previous.x, previous.y - pixel},
                                          {point.x, point.y - pixel}, shade.Get(), 1.5f);
                        context->DrawLine({previous.x, previous.y + pixel},
                                          {point.x, point.y + pixel}, shade.Get(), 1.5f);
                        context->DrawLine(previous, point, curveBrush.Get(), 1.5f);
                    }
                    previous = point;
                }
            }
        }

        for (int index = 0; index < 4; ++index) {
            const EdgeCurve& edited = curve(index);
            for (std::size_t at = 0; at < static_cast<std::size_t>(EdgeCurve::kPoints); ++at) {
                const D2D1_ELLIPSE circle{{edited.x[at] * width_, edited.y[at] * height_},
                                          kGripRadius, kGripRadius};
                context->FillEllipse(circle, fill.Get());
                context->DrawEllipse(circle, ring.Get(), 1.5f);
            }
        }
    });
}

bool SkinWizard::gripAt(Point point, int& curveIndex, std::size_t& pointIndex) const {
    if (width_ <= 0.0f || height_ <= 0.0f) return false;

    const float reach = kGripReach * kGripReach;

    for (int index = 0; index < 4; ++index) {
        const EdgeCurve& edited = curve(index);
        for (std::size_t at = 0; at < static_cast<std::size_t>(EdgeCurve::kPoints); ++at) {
            const float dx = point.x - edited.x[at] * width_;
            const float dy = point.y - edited.y[at] * height_;
            if (dx * dx + dy * dy <= reach) {
                curveIndex = index;
                pointIndex = at;
                return true;
            }
        }
    }
    return false;
}

void SkinWizard::chooseAnother() {
    if (onChooseAnother) onChooseAnother();
}

void SkinWizard::exitWizard() {
    if (onExit) onExit();
}

void SkinWizard::saveRequested() {
    // У правки старой обложки копия и имя уже есть — сохранение идёт сразу,
    // без диалога. Имя спрашивается только у новой.
    if (!skin_.image.empty()) {
        if (onSave) onSave(skin_, image_);
        return;
    }
    beginNaming();
}

void SkinWizard::beginNaming() {
    nameBox_.value().text(skin_.name);   // у правки — прежнее имя, у новой пусто
    namePanel_.value().visibility(Visibility::Visible);
    nameBox_.value().focus(FocusState::Programmatic);
}

void SkinWizard::finishNaming(bool save) {
    if (!save) {
        // «Отмена» — остаёмся в мастере, ничего не потеряв: точки как стояли,
        // так и стоят.
        namePanel_.value().visibility(Visibility::Collapsed);
        root_.value().focus(FocusState::Programmatic);
        return;
    }

    const std::wstring name{
        reinterpret_cast<wchar_t const*>(nameBox_.value().text().c_str())};
    if (name.empty() || blank(name)) return;   // безымянную сохранять некуда

    Skin saved = skin_;
    saved.name = name;

    namePanel_.value().visibility(Visibility::Collapsed);
    if (onSave) onSave(std::move(saved), image_);
}

}  // namespace bukvitsa::reader
