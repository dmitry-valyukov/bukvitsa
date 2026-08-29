#pragma once
// Раскодирование картинки с диска — общая ступень подложки темы и мастера
// обложек: WIC доводит снимок до 32bppPBGRA, битмап устройства из него делает
// уже тот контекст, который будет рисовать.

#include <filesystem>

#include <wrl/client.h>

struct IWICFormatConverter;

namespace bukvitsa::reader {

/// Раскодированный снимок, готовый стать битмапом устройства, — или nullptr,
/// если файла нет или это не картинка. Фабрика WIC своя и на один вызов:
/// снимки загружаются по смене темы и по открытию мастера, а не в цикле.
Microsoft::WRL::ComPtr<IWICFormatConverter> decodeImage(const std::filesystem::path& path);

/// Каталог исполняемого файла — от него достраиваются пути ресурсов Assets.
/// Именно от модуля, а не от текущего каталога: тот зависит от того, откуда
/// читалку запустили.
std::filesystem::path exeDirectory();

}  // namespace bukvitsa::reader
