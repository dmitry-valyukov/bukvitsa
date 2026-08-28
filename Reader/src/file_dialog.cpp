#include "file_dialog.h"

#include <windows.h>
#include <shobjidl.h>

#include <wrl/client.h>

namespace bukvitsa::reader {

using Microsoft::WRL::ComPtr;

std::filesystem::path askForBook(HWND__* owner) {
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&dialog)))) {
        return {};
    }

    // Два фильтра, а не один: «все файлы» нужен затем, что книга с чужим
    // расширением встречается, а объяснять читателю, почему его файла не
    // видно, — плохой разговор.
    static const COMDLG_FILTERSPEC kFilters[] = {
        {L"Книги FB3", L"*.fb3"},
        {L"Все файлы", L"*.*"},
    };
    dialog->SetFileTypes(static_cast<UINT>(sizeof(kFilters) / sizeof(kFilters[0])), kFilters);
    dialog->SetTitle(L"Добавить книгу");

    // Своё имя папки: диалог помнит по нему, где читатель был в прошлый раз,
    // и не путает читалку с другими приложениями.
    static const GUID kClientId = {
        0x4b1c2f60, 0x8d4a, 0x4b9e, {0x9c, 0x1a, 0x36, 0x0d, 0x2f, 0x77, 0x58, 0x11}};
    dialog->SetClientGuid(kClientId);

    if (FAILED(dialog->Show(owner))) {
        return {};   // читатель отказался, или диалог не показался
    }

    ComPtr<IShellItem> item;
    if (FAILED(dialog->GetResult(&item))) return {};

    PWSTR text = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &text)) || !text) return {};

    std::filesystem::path path{text};
    ::CoTaskMemFree(text);
    return path;
}

}  // namespace bukvitsa::reader
