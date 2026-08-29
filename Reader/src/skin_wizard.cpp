#include <algorithm>
#include <cmath>
#include <vector>

#include <windows.h>

#include <d2d1_1.h>
#include <wincodec.h>

// Заголовки проекта после стандартных. Свой первым.
#include "skin_wizard.h"

#include "imaging.h"

#include "Panels.h"

namespace bukvitsa::reader {

using namespace wxl;

namespace {

// Обстановка мастера — те же цвета, что у панели читалки: диалог имени и
// кнопки не бумага, а инструмент.
constexpr uint32_t kChrome = 0xF21E1E22;
constexpr uint32_t kInk = 0xFFE8E4DC;
constexpr uint32_t kEdge = 0x33FFFFFF;

// Кнопки — как на стартовом экране: та же ширина, та же полупрозрачность,
// под ними должен просвечивать снимок.
constexpr float kButtonWidth = 300.0f;
constexpr float kRestingOpacity = 0.92f;

// Оверлей поверх снимка. Вертикали тихие, кривые — тёплый акцент: их видно и
// на светлой бумаге, и на тёмном столе, и они — то, ради чего мастер открыт.
constexpr D2D1_COLOR_F kLineColor{1.0f, 1.0f, 1.0f, 0.35f};
constexpr D2D1_COLOR_F kCurveColor{1.0f, 0.72f, 0.30f, 0.9f};
constexpr D2D1_COLOR_F kGripFill{1.0f, 1.0f, 1.0f, 0.95f};
constexpr D2D1_COLOR_F kGripRing{0.15f, 0.12f, 0.08f, 0.9f};

// Фон до того, как снимок раскодировался, — тёмное дерево стола.
constexpr D2D1_COLOR_F kVoid{0.08f, 0.05f, 0.02f, 1.0f};

constexpr float kGripRadius = 7.0f;   ///< рисуемый кружочек, DIP
constexpr float kGripReach = 12.0f;   ///< зона захвата: шире кружочка, промах злит
constexpr float kCurveStep = 4.0f;    ///< шаг ломаной, которой рисуется кривая

/// Пусто ли имя — пробелы не в счёт.
bool blank(std::wstring_view text) {
    return std::all_of(text.begin(), text.end(),
                       [](wchar_t c) { return c == L' ' || c == L'\t'; });
}

}  // namespace

SkinWizard::SkinWizard(const Compositor& compositor) : compositor_(compositor) {
    baseX_ = skin_.x;
    buildTree();
}

Button SkinWizard::overlayButton(std::wstring_view caption, void (SkinWizard::*handler)()) {
    using namespace wxl::dsl;

    auto button = Button{
        caption,
        width = kButtonWidth,
        height = 46.0f,
        FontWeight{600},
        Margin{0, 6},
        fontSize = 15,
        onClick = [this, handler](Object const&, RoutedEventArgs&) { (this->*handler)(); },
    };

    // Полупрозрачность — визуалом, как у стартового экрана, только без
    // анимации появления: мастер открывают действием, ждать ему нечего.
    ElementCompositionPreview::getElementVisual(button).opacity(kRestingOpacity);
    return button;
}

void SkinWizard::buildTree() {
    using namespace wxl::dsl;

    surfaceHost_ = Grid{};

    // Кнопки — по центру правого листа: правая из двух равных колонок, в ней
    // по центру обеих осей.
    auto buttons = StackPanel{
        hAlign.center,
        vAlign.center,
        overlayButton(L"Сохранить", &SkinWizard::beginNaming),
        overlayButton(L"Выбрать другое изображение", &SkinWizard::chooseAnother),
        overlayButton(L"Выйти из мастера обложек", &SkinWizard::exitWizard),
    };

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
        background = SolidColorBrush{ARGB{0xFF140D06}},
        surfaceHost_.value(),
        Columns{Grid{}, buttons},
        namePanel_.value(),
    };

    tree.add_onLoaded([this](Object const&, RoutedEventArgs&) {
        root_.value().focus(FocusState::Programmatic);
        if (resizeSurface()) redraw();
    });

    tree.add_onSizeChanged([this](Object const&, SizeChangedEventArgs&) {
        // Координаты в долях, поэтому смена размеров ничего не двигает по
        // существу — точки остаются на своих местах снимка.
        if (resizeSurface()) redraw();
    });

    tree.add_onPointerPressed([this](Object const&, PointerRoutedEventArgs& args) {
        const PointerPoint touch = args.getCurrentPoint(root_.value());
        if (!touch.properties().isLeftButtonPressed()) return;

        std::size_t index = 0;
        const Grip grip = gripAt(touch.position(), index);
        if (grip == Grip::None) return;

        drag_ = grip;
        dragIndex_ = index;
        args.handled(true);
    });

    tree.add_onPointerMoved([this](Object const&, PointerRoutedEventArgs& args) {
        const Point point = args.getCurrentPoint(root_.value()).position();

        Grip active = drag_;
        if (drag_ != Grip::None) {
            const std::size_t index = dragIndex_;

            // Каждая точка ходит строго по своей оси и в своих пределах:
            // края — по вертикали не дальше четверти высоты от кромки,
            // вертикали — вбок от начального места, не дотягиваясь до соседок.
            switch (drag_) {
                case Grip::Top:
                    skin_.top[index] = std::clamp(point.y / height_, 0.0f, kEdgeReach);
                    break;
                case Grip::Bottom:
                    skin_.bottom[index] =
                        std::clamp(point.y / height_, 1.0f - kEdgeReach, 1.0f);
                    break;
                case Grip::Line:
                    skin_.x[index] = std::clamp(point.x / width_, baseX_[index] - kLineReach,
                                                baseX_[index] + kLineReach);
                    break;
                case Grip::None: break;
            }
            redraw();
            args.handled(true);
        } else {
            std::size_t index = 0;
            active = gripAt(point, index);
        }

        // Курсор — каждое движение заново: WinUI возвращает свою стрелку, а
        // спросить курсор у элемента проекция не умеет. Макрос ресурса Windows
        // допустим здесь — спрашиваем саму Windows.
        if (active == Grip::Top || active == Grip::Bottom) {
            ::SetCursor(::LoadCursorW(nullptr, IDC_SIZENS));
        } else if (active == Grip::Line) {
            ::SetCursor(::LoadCursorW(nullptr, IDC_SIZEWE));
        }
    });

    tree.add_onPointerReleased([this](Object const&, PointerRoutedEventArgs& args) {
        if (drag_ == Grip::None) return;
        drag_ = Grip::None;
        args.handled(true);
    });

    root_ = tree;
}

bool SkinWizard::open(std::filesystem::path image) {
    Microsoft::WRL::ComPtr<IWICFormatConverter> decoded = decodeImage(image);
    if (!decoded) return false;

    image_ = std::move(image);
    source_ = std::move(decoded);
    bitmap_.Reset();

    // Новый снимок — новая настройка: точки встают на начальные места.
    skin_ = defaultSkin();
    baseX_ = skin_.x;
    drag_ = Grip::None;
    namePanel_.value().visibility(Visibility::Collapsed);

    redraw();
    return true;
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

        context->Clear(kVoid);

        if (!bitmap_ && source_) {
            if (FAILED(context->CreateBitmapFromWicBitmap(source_.Get(), nullptr, &bitmap_)))
                source_.Reset();   // не вышло — больше не пытаемся
        }
        if (bitmap_) {
            // На всё окно, без сохранения пропорций — ровно так снимок ляжет
            // под страницу, и кривые надо снимать с него в этом же виде.
            context->DrawBitmap(bitmap_.Get(), D2D1::RectF(0.0f, 0.0f, width_, height_), 1.0f,
                                D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        }

        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> line;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> curve;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> fill;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> ring;
        context->CreateSolidColorBrush(kLineColor, &line);
        context->CreateSolidColorBrush(kCurveColor, &curve);
        context->CreateSolidColorBrush(kGripFill, &fill);
        context->CreateSolidColorBrush(kGripRing, &ring);
        if (!line || !curve || !fill || !ring) return;

        for (std::size_t index = 0; index < static_cast<std::size_t>(Skin::kPoints); ++index) {
            const float x = skin_.x[index] * width_;
            context->DrawLine({x, 0.0f}, {x, height_}, line.Get(), 1.0f);
        }

        // Девять кривых: верхняя и нижняя проходят через свои точки, семь
        // между ними — интерполяцией. Это и есть предпросмотр изгиба: строки
        // будущей страницы лягут так же.
        const int columns = std::max(2, static_cast<int>(width_ / kCurveStep));
        const std::vector<float> top = sampleEdge(skin_.x, skin_.top, columns);
        const std::vector<float> bottom = sampleEdge(skin_.x, skin_.bottom, columns);

        for (int row = 0; row < Skin::kPoints; ++row) {
            const float share = static_cast<float>(row) / (Skin::kPoints - 1);
            const float base = kEdgeInset + share * (1.0f - 2.0f * kEdgeInset);

            D2D1_POINT_2F previous{};
            for (int column = 0; column < columns; ++column) {
                const auto at = static_cast<std::size_t>(column);
                const float deviation = (top[at] - kEdgeInset) * (1.0f - share) +
                                        (bottom[at] - (1.0f - kEdgeInset)) * share;
                const float u = (static_cast<float>(column) + 0.5f) / static_cast<float>(columns);
                const D2D1_POINT_2F point{u * width_, (base + deviation) * height_};

                if (column > 0) context->DrawLine(previous, point, curve.Get(), 1.5f);
                previous = point;
            }
        }

        auto grip = [&](float x, float y) {
            const D2D1_ELLIPSE circle{{x, y}, kGripRadius, kGripRadius};
            context->FillEllipse(circle, fill.Get());
            context->DrawEllipse(circle, ring.Get(), 1.5f);
        };

        for (std::size_t index = 0; index < static_cast<std::size_t>(Skin::kPoints); ++index) {
            const float x = skin_.x[index] * width_;
            grip(x, skin_.top[index] * height_);
            grip(x, skin_.bottom[index] * height_);
            grip(x, height_ * 0.5f);
        }
    });
}

SkinWizard::Grip SkinWizard::gripAt(Point point, std::size_t& index) const {
    if (width_ <= 0.0f || height_ <= 0.0f) return Grip::None;

    // Не near/far: это макросы Windows, и имя с ними не живёт.
    const float reach = kGripReach * kGripReach;
    auto hit = [&](float x, float y) {
        const float dx = point.x - x;
        const float dy = point.y - y;
        return dx * dx + dy * dy <= reach;
    };

    for (std::size_t at = 0; at < static_cast<std::size_t>(Skin::kPoints); ++at) {
        const float x = skin_.x[at] * width_;

        if (hit(x, skin_.top[at] * height_)) {
            index = at;
            return Grip::Top;
        }
        if (hit(x, skin_.bottom[at] * height_)) {
            index = at;
            return Grip::Bottom;
        }
        if (hit(x, height_ * 0.5f)) {
            index = at;
            return Grip::Line;
        }
    }
    return Grip::None;
}

void SkinWizard::chooseAnother() {
    if (onChooseAnother) onChooseAnother();
}

void SkinWizard::exitWizard() {
    if (onExit) onExit();
}

void SkinWizard::beginNaming() {
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

    Skin skin = skin_;
    skin.name = name;

    namePanel_.value().visibility(Visibility::Collapsed);
    if (onSave) onSave(std::move(skin), image_);
}

}  // namespace bukvitsa::reader
