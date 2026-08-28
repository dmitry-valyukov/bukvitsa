// Мелочи, общие для разбора тела и метаданных: значения атрибутов FB3.
//
// Здесь остаётся только то, что знает про FB3: что атрибута может не быть и
// какие слова формат пишет в его значении. Всё, что касается символов и чисел
// как таковых, берётся из wxl::text.
//
// Значение атрибута приходит проверенным: wxl.xml проверяет документ целиком,
// прежде чем его разбирать, и её дерево отдаёт wxl::text::u8_view. Сравнение с
// литералом от этого не меняется -- обёртка сравнивается с обычным текстом.

// Партиция, которую первичный интерфейс не переэкспортирует: это внутренняя
// кухня разбора, и потребителю модуля её видеть незачем.
export module bukvitsa.fb3:parse_helpers;

import std;
import wxl.text;

import :node;

export namespace bukvitsa::fb3::detail {

/// FB3 пишет истину и как `true`, и как `1`.
inline bool toBool(std::optional<wxl::text::u8_view> value, bool fallback = false) {
    if (!value) return fallback;
    return *value == "true" || *value == "1";
}

/// Целое из значения атрибута; числом должно быть всё значение целиком.
std::optional<int> toInt(std::optional<wxl::text::u8_view> value);

/// «20mm», «50%», «1.5em» — число и единица без пробела между ними.
/// Пересчётом в пиксели занимается вёрстка: только она знает кегль полосы.
Length toLength(std::optional<wxl::text::u8_view> value);

inline FloatMode toFloatMode(std::optional<wxl::text::u8_view> value) {
    if (!value) return FloatMode::None;
    if (*value == "left") return FloatMode::Left;
    if (*value == "right") return FloatMode::Right;
    if (*value == "center") return FloatMode::Center;
    return FloatMode::None;
}

inline Align toAlign(std::optional<wxl::text::u8_view> value) {
    if (!value) return Align::Inherit;
    if (*value == "left") return Align::Left;
    if (*value == "right") return Align::Right;
    if (*value == "center") return Align::Center;
    if (*value == "justify") return Align::Justify;
    return Align::Inherit;
}

inline VerticalAlign toVerticalAlign(std::optional<wxl::text::u8_view> value) {
    if (!value) return VerticalAlign::Inherit;
    if (*value == "top") return VerticalAlign::Top;
    if (*value == "middle") return VerticalAlign::Middle;
    if (*value == "bottom") return VerticalAlign::Bottom;
    return VerticalAlign::Inherit;
}

inline NoteRole toNoteRole(std::optional<wxl::text::u8_view> value) {
    if (!value) return NoteRole::Auto;
    if (*value == "footnote") return NoteRole::Footnote;
    if (*value == "endnote") return NoteRole::Endnote;
    if (*value == "comment") return NoteRole::Comment;
    if (*value == "other") return NoteRole::Other;
    return NoteRole::Auto;
}

inline NoteNumbering toNoteNumbering(std::optional<wxl::text::u8_view> value) {
    if (!value) return NoteNumbering::Arabic;
    if (*value == "i") return NoteNumbering::Roman;
    if (*value == "a") return NoteNumbering::Alpha;
    if (*value == "*") return NoteNumbering::Asterisk;
    if (*value == "keep") return NoteNumbering::Keep;
    return NoteNumbering::Arabic;
}

inline SectionOutput toSectionOutput(std::optional<wxl::text::u8_view> value) {
    if (!value) return SectionOutput::Default;
    if (*value == "trial") return SectionOutput::Trial;
    if (*value == "trial-only") return SectionOutput::TrialOnly;
    if (*value == "payed") return SectionOutput::Payed;
    return SectionOutput::Default;
}

}  // namespace bukvitsa::fb3::detail
