#include <windows.h>

#include <algorithm>

// Свой заголовок последним: он импортирует wxl.xml (см. store.h).
#include "store.h"

import wxl.text;

namespace bukvitsa::reader {

std::string xmlValue(const wxl::text::u16_view value) {
    return wxl::text::xml_escaped(value.to_utf8().chars());
}

std::string xmlValue(const std::wstring_view value) {
    // Здесь чинят по-настоящему, а не для порядка: через этот вызов уходят в
    // файл пути и имена файлов, а имя файла в Windows — просто последовательность
    // 16-битных чисел, и непарный суррогат в ней возможен. Взятый на веру, он
    // превратился бы в три байта, которых UTF-8 не знает, и при следующем
    // запуске wxl.xml отвергла бы весь файл — то есть реестр книг или настройки
    // пропали бы целиком из-за одного дурного имени.
    return xmlValue(wxl::text::u16_view(wxl::text::repaired(value)));
}

std::wstring attributeOf(const wxl::xml::node& element, std::string_view name) {
    const auto value = element.attribute(name);
    return value ? std::wstring(value->to_utf16().wchars()) : std::wstring{};
}

std::uint64_t numberOf(const wxl::xml::node& element, std::string_view name,
                       std::uint64_t fallback) {
    const auto value = element.attribute(name);
    if (!value) return fallback;

    // Числом должно быть всё значение: разобралось не до конца — значит там не
    // число, и лучше умолчание, чем половина прочитанного.
    return wxl::text::parse<std::uint64_t>(value->chars()).value_or(fallback);
}

double realOf(const wxl::xml::node& element, std::string_view name, double fallback) {
    const auto value = element.attribute(name);
    if (!value) return fallback;

    return wxl::text::parse<double>(value->chars()).value_or(fallback);
}


}  // namespace bukvitsa::reader
