#include "button_card.h"

namespace bukvitsa::reader {

using namespace wxl;

namespace {

// Лицо и рамка. Прозрачность сильная: под карточкой картинка, ей и жить, —
// поэтому ресурсные кисти библиотечной карточки здесь подменяются.
constexpr uint32_t kFace = 0x6C1C1208;
constexpr uint32_t kEdge = 0x33FFFFFF;

// Где стоит колонка кнопок: отступы стартового экрана, у мастера те же.
constexpr float kTopMargin = 64.0f;
constexpr float kRightMargin = 72.0f;

}  // namespace

Border buttonCard(const UIElement& content) {
    using namespace wxl::dsl;

    // Вид — от wxl::Card: скругление, волосяная рамка, тень и подъём по Z,
    // без которого тень лежит плоско. Настройки ниже идут после пресета и
    // потому сильнее его: место карточки и её лицо — дело читалки.
    return Built<Card>{
        hAlign.right,
        vAlign.top,
        Margin{0, kTopMargin, kRightMargin, 0},
        CornerRadius{12},
        background = SolidColorBrush{ARGB{kFace}},
        borderBrush = SolidColorBrush{ARGB{kEdge}},
        content,
    };
}

}  // namespace bukvitsa::reader
