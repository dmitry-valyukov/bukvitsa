#include "settings.h"

#include <windows.h>
#include <shlobj.h>



// Свои заголовки со стандартными внутри — до импорта: он несёт с собой
// модульный std, а стандартный заголовок после него MSVC уже не принимает.
#include "store.h"

import wxl.text;
import wxl.xml;

namespace bukvitsa::reader {

std::filesystem::path dataDirectory() {
    PWSTR folder = nullptr;
    if (FAILED(::SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &folder))) {
        return {};
    }
    std::filesystem::path path{folder};
    ::CoTaskMemFree(folder);
    return path / L"Bukvitsa" / L"Reader";
}

Settings loadSettings() {
    Settings settings;

    const std::filesystem::path path = dataDirectory() / L"settings.xml";
    std::error_code ignored;
    if (path.empty() || !std::filesystem::exists(path, ignored)) {
        return settings;   // первый запуск
    }

    try {
        wxl::xml::document document;
        const wxl::xml::node& root = document.load_file(path);

        if (const wxl::xml::node* window = root.child("window")) {
            settings.windowPlacement = attributeOf(*window, "placement");
        }
        if (const wxl::xml::node* reading = root.child("reading")) {
            settings.continueReading = reading->attribute("continue") == "true";
        }
        const auto version = static_cast<int>(numberOf(root, "version", Settings::kVersion));

        if (const wxl::xml::node* text = root.child("text")) {
            settings.theme = static_cast<int>(numberOf(*text, "theme", 0));
            settings.fontSize = static_cast<float>(realOf(*text, "fontSize", 20.0));
            settings.lineHeight = static_cast<float>(realOf(*text, "lineHeight", 1.45));
            settings.margin = static_cast<float>(realOf(*text, "margin", 3.0));

            // Первая версия писала интерлиньяж и поля сотыми долями целым
            // числом — обходом разбора дробного, который зависел бы от
            // локали. Обхода больше нет: ни запись, ни разбор локали не знают.
            // А файлы с ним остались — их ровно столько, сколько машин, где
            // читалка уже запускалась, и молча сбрасывать на них вид не за что.
            if (version < 2) {
                settings.lineHeight /= 100.0f;
                settings.margin /= 100.0f;
            }
        }
        if (const wxl::xml::node* book = root.child("lastBook")) {
            settings.lastBookGuid = attributeOf(*book, "guid");
            settings.lastBookPath = attributeOf(*book, "path");
        }
    } catch (...) {
        // Битый файл, файл от будущей версии, файл, который правили руками, —
        // всё это повод открыться со значениями по умолчанию, а не повод не
        // открыться. Настройки не стоят отказа запускаться.
        return Settings{};
    }

    return settings;
}

bool saveSettings(const Settings& settings) {
    // text_builder, а не поток с нейтральной локалью: локали у него нет вовсе,
    // и дробное число пишется точкой, какие бы настройки ни стояли в Windows.
    wxl::text::text_builder<> out;

    out.append("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n");
    out.format("<settings version=\"{}\">\n", Settings::kVersion);
    out.format("  <window placement=\"{}\"/>\n", xmlValue(settings.windowPlacement));
    out.format("  <reading continue=\"{}\"/>\n", settings.continueReading ? "true" : "false");
    out.format("  <text theme=\"{}\" fontSize=\"{}\" lineHeight=\"{}\" margin=\"{}\"/>\n",
               settings.theme, settings.fontSize, settings.lineHeight, settings.margin);

    if (!settings.lastBookGuid.empty()) {
        out.format("  <lastBook guid=\"{}\" path=\"{}\"/>\n", xmlValue(settings.lastBookGuid),
                   xmlValue(settings.lastBookPath));
    }

    out.append("</settings>\n");

    const std::filesystem::path directory = dataDirectory();
    if (directory.empty()) return false;
    return writeFile(directory / L"settings.xml", out.view());
}

}  // namespace bukvitsa::reader
