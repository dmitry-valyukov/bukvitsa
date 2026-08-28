#pragma once
// Bukvitsa.Typography — вёрстка книжного текста поверх DirectWrite.
//
// Планируемые модули:
//   shaper.h     — итемизация/шейпинг (IDWriteTextAnalyzer), фолбэк шрифтов
//   hyphenator.h — переносы (hunspell-hyphen, алгоритм Ляна; ru/en словари)
//   linebreak.h  — разбивка на строки: greedy+переносы, затем Кнут-Пласс
//   justify.h    — выключка (IDWriteTextAnalyzer1::GetJustificationOpportunities...)
//   paginator.h  — страницы: layout от якоря чтения, фоновая пагинация
//   position.h   — позиции чтения/закладок: xpointer-стиль против исходной
//                  структуры FB3, с версией формата с первого дня
//
// Принципы: абзац — единица шейпинга; страница — список (абзац, диапазон строк);
// все размеры в DIP; Per-Monitor DPI v2.

// Через block.h, а не своим: довод там же.
#include "bukvitsa/typography/block.h"

namespace bukvitsa::typography {

inline constexpr std::uint32_t kVersion = 0x0000'0100; // 0.1.0

} // namespace bukvitsa::typography
