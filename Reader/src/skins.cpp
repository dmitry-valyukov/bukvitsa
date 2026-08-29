#include <algorithm>
#include <cmath>
#include <utility>

// Заголовки проекта после стандартных: store.h несёт импорт, после которого
// стандартный заголовок MSVC уже не принимает. Свой первым.
#include "skins.h"

#include "settings.h"
#include "store.h"

namespace bukvitsa::reader {

namespace {

/// Кривая с точками на прямой: равномерно от `from` до `to` по X, все на
/// одной высоте `level`.
EdgeCurve straightCurve(float from, float to, float level) {
    EdgeCurve curve;

    const float step = (to - from) / (EdgeCurve::kPoints - 1);
    for (int index = 0; index < EdgeCurve::kPoints; ++index) {
        curve.x[static_cast<std::size_t>(index)] = from + step * static_cast<float>(index);
        curve.y[static_cast<std::size_t>(index)] = level;
    }
    return curve;
}

/// Разбирает одну кривую из дочерних <point>. Чего в файле нет или что
/// вылезло из долей — остаётся от начальной прямой: файл могли поправить
/// руками, и это не повод ронять обложку.
EdgeCurve curveOf(const wxl::xml::node& element, EdgeCurve fallback) {
    EdgeCurve curve = fallback;

    std::size_t index = 0;
    for (const wxl::xml::node& point : element.children_named("point")) {
        if (index >= static_cast<std::size_t>(EdgeCurve::kPoints)) break;
        curve.x[index] =
            std::clamp(static_cast<float>(realOf(point, "x", curve.x[index])), 0.0f, 1.0f);
        curve.y[index] =
            std::clamp(static_cast<float>(realOf(point, "y", curve.y[index])), 0.0f, 1.0f);
        ++index;
    }
    return curve;
}

void writeCurve(wxl::text::text_builder<>& out, const char* name, const EdgeCurve& curve) {
    out.format("    <{}>\n", name);
    for (std::size_t index = 0; index < static_cast<std::size_t>(EdgeCurve::kPoints); ++index) {
        out.format("      <point x=\"{}\" y=\"{}\"/>\n", curve.x[index], curve.y[index]);
    }
    out.format("    </{}>\n", name);
}

}  // namespace

Skin defaultSkin() {
    Skin skin;

    skin.topLeft = straightCurve(kEdgeInset, 0.5f - kEdgeInset, kEdgeInset);
    skin.topRight = straightCurve(0.5f + kEdgeInset, 1.0f - kEdgeInset, kEdgeInset);
    skin.bottomLeft = straightCurve(kEdgeInset, 0.5f - kEdgeInset, 1.0f - kEdgeInset);
    skin.bottomRight = straightCurve(0.5f + kEdgeInset, 1.0f - kEdgeInset, 1.0f - kEdgeInset);
    return skin;
}

float edgeAt(const EdgeCurve& curve, float u) {
    constexpr std::size_t last = static_cast<std::size_t>(EdgeCurve::kPoints) - 1;

    if (u <= curve.x[0]) return curve.y[0];
    if (u >= curve.x[last]) return curve.y[last];

    // Кривая Безье четвёртой степени: пять точек мастера — её управляющая
    // ломаная. Кривая проходит только через крайние точки, средние тянут её к
    // себе — зато она не выскакивает за свою ломаную, как это делал между
    // точками Катмулл-Ром, и край выходит спокойным при любой расстановке.
    const auto at = [&curve](float t) {
        const float s = 1.0f - t;
        const float w0 = s * s * s * s;
        const float w1 = 4.0f * s * s * s * t;
        const float w2 = 6.0f * s * s * t * t;
        const float w3 = 4.0f * s * t * t * t;
        const float w4 = t * t * t * t;
        return std::pair{w0 * curve.x[0] + w1 * curve.x[1] + w2 * curve.x[2] + w3 * curve.x[3] +
                             w4 * curve.x[4],
                         w0 * curve.y[0] + w1 * curve.y[1] + w2 * curve.y[2] + w3 * curve.y[3] +
                             w4 * curve.y[4]};
    };

    // Кривая параметрическая, а спрашивают её по столбцу u, поэтому t ищется
    // по x бисекцией: иксы точек идут по порядку (клампы перетаскивания это
    // держат), значит x(t) монотонен. Двадцать делений — миллионная доля
    // ширины, карте хватает с запасом; замкнутой формулы у корня четвёртой
    // степени всё равно нет.
    float low = 0.0f;
    float high = 1.0f;
    for (int step = 0; step < 20; ++step) {
        const float mid = 0.5f * (low + high);
        if (at(mid).first < u) {
            low = mid;
        } else {
            high = mid;
        }
    }
    return at(0.5f * (low + high)).second;
}

void Skins::loadFrom(std::string xml) {
    skins_.clear();

    if (xml.empty()) return;

    try {
        wxl::xml::document document;
        const wxl::xml::node& root = document.load(std::move(xml));

        for (const wxl::xml::node& element : root.children_named("skin")) {
            Skin skin = defaultSkin();
            skin.name = attributeOf(element, "name");
            skin.image = attributeOf(element, "image");

            if (const wxl::xml::node* curve = element.child("topLeft"))
                skin.topLeft = curveOf(*curve, skin.topLeft);
            if (const wxl::xml::node* curve = element.child("topRight"))
                skin.topRight = curveOf(*curve, skin.topRight);
            if (const wxl::xml::node* curve = element.child("bottomLeft"))
                skin.bottomLeft = curveOf(*curve, skin.bottomLeft);
            if (const wxl::xml::node* curve = element.child("bottomRight"))
                skin.bottomRight = curveOf(*curve, skin.bottomRight);

            // Обложка без имени не выбирается, без снимка не рисуется; такого
            // в файле, который писали мы, не бывает — но файл могли и
            // поправить руками.
            if (!skin.name.empty() && !skin.image.empty()) skins_.push_back(std::move(skin));
        }
    } catch (...) {
        // Битый реестр — пустой список обложек, а не отказ запуститься:
        // встроенные темы никуда не деваются.
        skins_.clear();
    }
}

std::string Skins::toXml() const {
    wxl::text::text_builder<> out;

    out.append("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n");
    out.format("<skins version=\"{}\">\n", kVersion);

    for (const Skin& skin : skins_) {
        out.format("  <skin name=\"{}\" image=\"{}\">\n", xmlValue(skin.name),
                   xmlValue(skin.image));
        writeCurve(out, "topLeft", skin.topLeft);
        writeCurve(out, "topRight", skin.topRight);
        writeCurve(out, "bottomLeft", skin.bottomLeft);
        writeCurve(out, "bottomRight", skin.bottomRight);
        out.append("  </skin>\n");
    }

    out.append("</skins>\n");
    return std::string(out.view());
}

const Skin* Skins::find(std::wstring_view name) const {
    for (const Skin& skin : skins_) {
        if (skin.name == name) return &skin;
    }
    return nullptr;
}

void Skins::put(Skin skin) {
    for (Skin& known : skins_) {
        if (known.name == skin.name) {
            known = std::move(skin);
            return;
        }
    }
    skins_.push_back(std::move(skin));
}

std::filesystem::path skinsPath() {
    const std::filesystem::path directory = dataDirectory();

    return directory.empty() ? std::filesystem::path{} : directory / L"skins.xml";
}

std::filesystem::path skinDirectory() {
    const std::filesystem::path directory = dataDirectory();

    return directory.empty() ? std::filesystem::path{} : directory / L"skins";
}

}  // namespace bukvitsa::reader
