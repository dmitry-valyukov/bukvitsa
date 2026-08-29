#include <algorithm>
#include <cmath>

// Заголовки проекта после стандартных: store.h несёт импорт, после которого
// стандартный заголовок MSVC уже не принимает. Свой первым.
#include "skins.h"

#include "settings.h"
#include "store.h"

namespace bukvitsa::reader {

Skin defaultSkin() {
    Skin skin;

    constexpr float step = (1.0f - 2.0f * kEdgeInset) / (Skin::kPoints - 1);
    for (int index = 0; index < Skin::kPoints; ++index) {
        skin.x[static_cast<std::size_t>(index)] = kEdgeInset + step * static_cast<float>(index);
        skin.top[static_cast<std::size_t>(index)] = kEdgeInset;
        skin.bottom[static_cast<std::size_t>(index)] = 1.0f - kEdgeInset;
    }
    return skin;
}

std::vector<float> sampleEdge(std::span<const float> xs, std::span<const float> ys, int columns) {
    std::vector<float> out(static_cast<std::size_t>(columns));
    const std::size_t last = xs.size() - 1;

    // Колонки идут по возрастанию, поэтому сегмент только движется вперёд и
    // весь проход линейный.
    std::size_t segment = 0;

    for (int column = 0; column < columns; ++column) {
        const float u = (static_cast<float>(column) + 0.5f) / static_cast<float>(columns);
        float value;

        if (u <= xs[0]) {
            value = ys[0];
        } else if (u >= xs[last]) {
            value = ys[last];
        } else {
            while (u > xs[segment + 1]) ++segment;

            // Катмулл-Ром по четвёрке соседей; у крайних сегментов сосед за
            // краем — сама крайняя точка, обычное «зажатие» концов.
            const float t = (u - xs[segment]) / (xs[segment + 1] - xs[segment]);
            const float p0 = ys[segment == 0 ? 0 : segment - 1];
            const float p1 = ys[segment];
            const float p2 = ys[segment + 1];
            const float p3 = ys[std::min(segment + 2, last)];

            value = 0.5f * (2.0f * p1 + (-p0 + p2) * t +
                            (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t * t +
                            (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t * t * t);
        }

        out[static_cast<std::size_t>(column)] = value;
    }
    return out;
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

            // Все координаты — доли, и файл могли поправить руками: значение
            // вне [0, 1] прижимается, а не роняет обложку.
            std::size_t index = 0;
            for (const wxl::xml::node& line : element.children_named("line")) {
                if (index >= static_cast<std::size_t>(Skin::kPoints)) break;
                skin.x[index] =
                    std::clamp(static_cast<float>(realOf(line, "x", skin.x[index])), 0.0f, 1.0f);
                skin.top[index] = std::clamp(
                    static_cast<float>(realOf(line, "top", skin.top[index])), 0.0f, 1.0f);
                skin.bottom[index] = std::clamp(
                    static_cast<float>(realOf(line, "bottom", skin.bottom[index])), 0.0f, 1.0f);
                ++index;
            }

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
        for (std::size_t index = 0; index < static_cast<std::size_t>(Skin::kPoints); ++index) {
            out.format("    <line x=\"{}\" top=\"{}\" bottom=\"{}\"/>\n", skin.x[index],
                       skin.top[index], skin.bottom[index]);
        }
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
