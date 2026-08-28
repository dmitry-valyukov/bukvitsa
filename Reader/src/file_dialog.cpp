#include "file_dialog.h"

#include <windows.h>
#include <shobjidl.h>

#include <wrl/client.h>

namespace bukvitsa::reader {

using Microsoft::WRL::ComPtr;

namespace {

/// Своё имя папки: диалог помнит по нему, где читатель был в прошлый раз, и не
/// путает читалку с другими приложениями. Одно на оба диалога -- книгу и
/// каталог с книгами ищут в одних и тех же местах.
const GUID kClientId = {
    0x4b1c2f60, 0x8d4a, 0x4b9e, {0x9c, 0x1a, 0x36, 0x0d, 0x2f, 0x77, 0x58, 0x11}};

/// То общее, что есть у обоих: показать диалог и достать из него путь.
std::filesystem::path showAndTake(IFileOpenDialog& dialog, HWND__* owner) {
    if (FAILED(dialog.Show(owner))) return {};   // отказались, или не показался

    ComPtr<IShellItem> item;
    if (FAILED(dialog.GetResult(&item))) return {};

    PWSTR text = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &text)) || !text) return {};

    std::filesystem::path path{text};
    ::CoTaskMemFree(text);
    return path;
}

}  // namespace

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
    dialog->SetClientGuid(kClientId);

    return showAndTake(*dialog.Get(), owner);
}

std::filesystem::path askForFolder(HWND__* owner) {
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&dialog)))) {
        return {};
    }

    // Тот же диалог, что и для книги, с одним поднятым флагом: у Windows
    // выбор папки — это выбор файла, которому разрешили быть папкой.
    FILEOPENDIALOGOPTIONS options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_PATHMUSTEXIST);

    dialog->SetTitle(L"Каталог с книгами");
    dialog->SetClientGuid(kClientId);

    return showAndTake(*dialog.Get(), owner);
}

}  // namespace bukvitsa::reader
