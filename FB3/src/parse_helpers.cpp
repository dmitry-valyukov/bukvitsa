// Те два разбора значений атрибутов, которым нужен wxl::text.
//
// Отдельным файлом, а не в заголовке: разбор числа — это уже не про FB3, а про
// цифры, и держать его рядом с таблицей слов формата незачем.

module bukvitsa.fb3;

import std;
import wxl.text;

import :parse_helpers;

namespace bukvitsa::fb3::detail {

std::optional<int> toInt(const std::optional<wxl::text::u8_view> value) {
    if (!value) return std::nullopt;

    // Числом должно быть всё значение: «12abc» — это не двенадцать, это
    // испорченный атрибут, и лучше о нём не знать вовсе, чем знать половину.
    return wxl::text::parse<int>(value->chars());
}

Length toLength(const std::optional<wxl::text::u8_view> value) {
    Length length;

    if (!value) return length;

    // Число и то, что за ним, одним разбором: parse_prefix отдаёт и значение,
    // и хвост, в котором стоит единица.
    const auto parsed = wxl::text::parse_prefix<float>(value->chars());

    if (!parsed) return length;

    if (parsed->rest == "mm") length.unit = Length::Unit::Millimeter;
    else if (parsed->rest == "%") length.unit = Length::Unit::Percent;
    else if (parsed->rest == "em") length.unit = Length::Unit::Em;
    else if (parsed->rest == "ex") length.unit = Length::Unit::Ex;
    else return length;  // единицы, которой формат не знает, всё равно что нет

    length.value = parsed->value;
    return length;
}

}  // namespace bukvitsa::fb3::detail
