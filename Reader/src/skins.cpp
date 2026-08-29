#include <algorithm>
#include <cmath>

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

    std::size_t segment = 0;
    while (u > curve.x[segment + 1]) ++segment;

    // Катмулл-Ром по четвёрке соседей; у крайних сегментов сосед за краем —
    // сама крайняя точка, обычное «зажатие» концов.
    const float t = (u - curve.x[segment]) / (curve.x[segment + 1] - curve.x[segment]);
    const float p0 = curve.y[segment == 0 ? 0 : segment - 1];
    const float p1 = curve.y[segment];
    const float p2 = curve.y[segment + 1];
    const float p3 = curve.y[std::min(segment + 2, last)];

    return 0.5f * (2.0f * p1 + (-p0 + p2) * t + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t * t +
                   (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t * t * t);
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
