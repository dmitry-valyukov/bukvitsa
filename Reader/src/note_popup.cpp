#include <algorithm>

// Заголовки проекта после всех стандартных: они ведут к импорту модуля книги,
// а стандартный заголовок после импорта MSVC уже не принимает. Свой первым:
// он единственный тянет за собой стандартные заголовки, которых нет здесь.
#include "note_popup.h"

#include "bukvitsa/typography/block.h"
#include "bukvitsa/typography/glyph_painter.h"

namespace bukvitsa::reader {

using namespace wxl;

namespace {

constexpr float kPadding = 16.0f;
constexpr float kCorner = 10.0f;
constexpr float kMaxWidth = 460.0f;
constexpr float kGapFromAnchor = 10.0f;

/// Доля высоты полосы, выше которой всплывашка не растёт. Сноска, занявшая
/// пол-экрана, перестаёт быть примечанием и становится второй страницей — а
/// прокрутка внутри неё дешевле, чем потерянное место чтения.
constexpr float kMaxHeightShare = 0.55f;

/// Цвет D2D в цвет XAML: подложку рисует не наша поверхность, а Border, —
/// у него уже есть и скругление, и рамка, и незачем чертить их руками.
Color toXaml(D2D1_COLOR_F color) {
    auto byte = [](float value) {
        return static_cast<uint8_t>(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
    };
    return Color{byte(color.a), byte(color.r), byte(color.g), byte(color.b)};
}

}  // namespace

NotePopup::NotePopup(const Compositor& compositor) : compositor_(compositor) {
    using namespace wxl::dsl;

    // Подложка ростом с текст сноски: она внутри прокрутки, и её высота — это
    // высота сноски. Сам текст живёт на поверхности композитора, привязанной
    // к этому элементу, поэтому прокрутка двигает его вместе с ним.
    paper_ = Grid{};
    sprite_ = compositor_.createSpriteVisual();
    ElementCompositionPreview::setElementChildVisual(paper_.value(), sprite_.value());

    scroll_ = ScrollViewer{
        horizontalScrollBarVisibility = ScrollBarVisibility::Disabled,
        verticalScrollBarVisibility = ScrollBarVisibility::Auto,
        content = paper_.value(),
    };

    root_ = Border{
        hAlign.left,
        vAlign.top,
        visibility = Visibility::Collapsed,
        CornerRadius{kCorner},
        BorderThickness{1},
        Padding{kPadding},
        scroll_.value(),
    };
}

void NotePopup::hide() {
    if (!visible_) return;
    visible_ = false;
    root_.value().visibility(Visibility::Collapsed);
}

float NotePopup::layout(Book& book, const fb3::Node* note, float width, float fontSize) {
    lines_.clear();
    baselines_.clear();

    if (!note || width <= 0.0f) return 0.0f;

    typography::Engine& engine = book.engine();
    float y = 0.0f;

    for (const typography::Block& block : typography::flatten(*note)) {
        if (block.paragraph.text.empty()) continue;

        // Сноска набирается мельче книги и без абзацного отступа: она короткая,
        // и отступ в ней читался бы как случайная дыра.
        typography::ParagraphStyle style;
        style.fontSize = fontSize * 0.85f;
        style.lineHeight = 1.35f;
        style.alignment = typography::Alignment::Justify;

        for (typography::Line& line : engine.layout(block.paragraph, width, style)) {
            y += line.ascent;
            baselines_.push_back(y);
            y += line.height - line.ascent;
            lines_.push_back(std::move(line));
        }

        y += style.fontSize * 0.4f;   // отбивка между абзацами сноски
    }

    return y;
}

void NotePopup::draw(const Theme& theme, float width, float height, float scale) {
    const SizeInt32 pixels{static_cast<int32_t>(width * scale + 0.5f),
                           static_cast<int32_t>(height * scale + 0.5f)};
    if (pixels.width <= 0 || pixels.height <= 0) return;

    if (surface_) {
        surface_->resize(pixels);
    } else {
        surface_.emplace(compositor_, pixels);
        sprite_.value().brush(surface_->brush());
    }
    sprite_.value().size({width, height});

    surface_->draw([&](ID2D1DeviceContext* context) {
        D2D1_MATRIX_3X2_F atlas{};
        context->GetTransform(&atlas);
        context->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale) *
                              *D2D1::Matrix3x2F::ReinterpretBaseType(&atlas));

        context->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        context->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        context->Clear(D2D1::ColorF(0, 0, 0, 0));   // подложку рисует Border

        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> ink;
        context->CreateSolidColorBrush(theme.text, &ink);
        if (!ink) return;

        for (std::size_t i = 0; i < lines_.size(); ++i) {
            typography::drawLine(context, lines_[i], 0.0f, baselines_[i], ink.Get());
        }
    });
}

void NotePopup::show(Book& book, const fb3::Node* note, Point anchor, Size area, const Theme& theme,
                     float fontSize, float scale) {
    const float width = std::min(kMaxWidth, std::max(area.width * 0.6f, 220.0f));
    const float inner = width - kPadding * 2.0f;

    const float noteHeight = layout(book, note, inner, fontSize);
    if (lines_.empty()) {
        hide();
        return;
    }

    const float height =
        std::min(noteHeight + kPadding * 2.0f, std::max(area.height * kMaxHeightShare, 120.0f));

    // Встать под знаком сноски, а если внизу не помещается — над ним. Так
    // всплывашка не закрывает того места, ради которого её открыли.
    float x = std::clamp(anchor.x - width * 0.5f, 8.0f, std::max(area.width - width - 8.0f, 8.0f));
    float y = anchor.y + kGapFromAnchor;
    if (y + height > area.height - 8.0f) {
        y = std::max(anchor.y - height - kGapFromAnchor, 8.0f);
    }

    root_.value().margin({x, y, 0, 0});
    root_.value().width(width);
    root_.value().height(height);
    root_.value().background(SolidColorBrush{toXaml(theme.panel)});
    root_.value().borderBrush(SolidColorBrush{toXaml(theme.dim)});

    paper_.value().height(noteHeight);
    scroll_.value().changeView(std::nullopt, 0.0, std::nullopt);

    draw(theme, inner, noteHeight, scale);

    root_.value().visibility(Visibility::Visible);
    visible_ = true;
}

}  // namespace bukvitsa::reader
