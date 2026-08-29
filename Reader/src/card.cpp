#include "card.h"

namespace bukvitsa::reader {

using namespace wxl;

namespace {

// Лицо и рамка. Прозрачность сильная: под карточкой картинка, ей и жить.
constexpr uint32_t kFace = 0x6C1C1208;
constexpr uint32_t kEdge = 0x33FFFFFF;

// Где стоит колонка кнопок: отступы стартового экрана, у мастера те же.
constexpr float kTopMargin = 64.0f;
constexpr float kRightMargin = 72.0f;

}  // namespace

Border buttonCard(const UIElement& content) {
    using namespace wxl::dsl;

    return Border{
        hAlign.right,
        vAlign.top,
        Margin{0, kTopMargin, kRightMargin, 0},
        CornerRadius{12},
        background = SolidColorBrush{ARGB{kFace}},
        borderBrush = SolidColorBrush{ARGB{kEdge}},
        BorderThickness{1},
        // Тень считается от подъёма по Z; без него она лежит плоско под
        // карточкой и не видна.
        shadow = ThemeShadow{},
        translation = {0.0f, 0.0f, 32.0f},
        Padding{16},
        content,
    };
}

}  // namespace bukvitsa::reader
