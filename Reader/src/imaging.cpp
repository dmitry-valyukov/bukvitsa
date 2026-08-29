#include "imaging.h"

#include <windows.h>

#include <wincodec.h>

namespace bukvitsa::reader {

using Microsoft::WRL::ComPtr;

ComPtr<IWICFormatConverter> decodeImage(const std::filesystem::path& path) {
    ComPtr<IWICImagingFactory> wic;
    if (FAILED(::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&wic))))
        return nullptr;

    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                              WICDecodeMetadataCacheOnLoad, &decoder)))
        return nullptr;

    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame))) return nullptr;

    ComPtr<IWICFormatConverter> converter;
    if (FAILED(wic->CreateFormatConverter(&converter))) return nullptr;
    if (FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA,
                                     WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeMedianCut)))
        return nullptr;

    return converter;
}

std::filesystem::path exeDirectory() {
    wchar_t module[MAX_PATH];
    if (::GetModuleFileNameW(nullptr, module, MAX_PATH) == 0) return {};

    return std::filesystem::path(module).parent_path();
}

}  // namespace bukvitsa::reader
