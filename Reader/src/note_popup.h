#pragma once
// Всплывающая сноска.
//
// Сноски внизу полосы отложены не из лени: у них высота полосы зависит от
// того, что на неё попало, а что на неё попало — от высоты полосы, и пагинация
// перестаёт быть одним проходом. Всплывающая сноска этой связи не создаёт
// вовсе: страница уже сверстана, а сноска показывается поверх неё по щелчку и
// исчезает по Escape. Читателю она к тому же удобнее — не нужно искать глазами
// низ полосы и возвращаться обратно.
//
// Внутри — тот же движок вёрстки, что и у книги: сноска набирается по тем же
// правилам, только уже и мельче. Не поместилась — прокручивается.

// Свои заголовки со стандартными внутри — до всего, что тянет import
// wxl.core.
#include <functional>
#include <optional>
#include <vector>

#include "theme.h"

#include "DrawingSurface.h"
#include "Object.h"
#include "pch.h"

// Последним: он ведёт к модели книги, а она импортирует wxl.text, после чего
// стандартный заголовок MSVC уже не принимает.
#include "book.h"

namespace bukvitsa::reader {

class NotePopup {
public:
    explicit NotePopup(const wxl::Compositor& compositor);

    /// Элемент, который кладут поверх полосы набора.
    const wxl::UIElement& root() const { return root_.value(); }

    /// Показывает тело сноски, стараясь встать рядом с её знаком.
    ///
    /// @param anchor низ знака сноски в координатах полосы, DIP.
    /// @param area   размер полосы: в него всплывашка и вписывается.
    /// @param scale  масштаб экрана: поверхность живёт в пикселях.
    void show(Book& book, const fb3::Node* note, wxl::Point anchor, wxl::Size area,
              const Theme& theme, float fontSize, float scale);

    void hide();
    bool visible() const { return visible_; }

private:
    /// Верстает тело сноски и возвращает её высоту.
    float layout(Book& book, const fb3::Node* note, float width, float fontSize);
    void draw(const Theme& theme, float width, float height, float scale);

    wxl::Compositor compositor_;
    wxl::core::nullable<wxl::Border> root_ = nullptr;
    wxl::core::nullable<wxl::ScrollViewer> scroll_ = nullptr;
    wxl::core::nullable<wxl::Grid> paper_ = nullptr;   ///< подложка ростом с текст сноски
    wxl::core::nullable<wxl::SpriteVisual> sprite_ = nullptr;
    std::optional<wxl::DrawingSurface> surface_;

    std::vector<typography::Line> lines_;
    std::vector<float> baselines_;
    bool visible_ = false;
};

}  // namespace bukvitsa::reader
