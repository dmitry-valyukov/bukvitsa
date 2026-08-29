#pragma once
// Мастер обложек: читатель укладывает кривые по краям листа своего снимка.
//
// Мастер показывает выбранный снимок во всё окно — растянутым ровно так, как
// его растянет тема, — и поверх него сетку управления: девять вертикалей с
// точками верхнего и нижнего края и девять горизонтальных кривых, живой
// предпросмотр будущего изгиба строк. Точки тянутся мышью: крайние — строго
// по вертикали, серединные на вертикалях — по горизонтали. Все координаты
// живут в долях ширины и высоты, поэтому смена размеров окна ничего не
// теряет.
//
// Как и остальные экраны, класс не наследуется от визуальных компонент, а
// держит их внутри себя: корень отдаётся окну как содержимое. Диска здесь
// нет: снимок мастер только раскодирует, а копию кладёт приложение — через
// свой рабочий поток.

#include <array>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include <wrl/client.h>

#include "skins.h"

#include "DrawingSurface.h"
#include "Nullable.h"
#include "pch.h"

struct IWICFormatConverter;
struct ID2D1Bitmap1;

namespace bukvitsa::reader {

class SkinWizard {
public:
    explicit SkinWizard(const wxl::Compositor& compositor);

    /// Корень, который отдаётся окну как содержимое.
    const wxl::UIElement& root() const { return root_.value(); }

    /// Открывает мастер на этом снимке: точки встают на начальные места.
    /// false — файл не раскодировался как картинка, и показывать нечего.
    bool open(std::filesystem::path image);

    /// «Сохранить», имя уже введено и не пустое. Обложка приходит с пустым
    /// `image`: под каким именем ляжет копия снимка, решает приложение.
    /// Второй аргумент — исходный файл, с которого копию снимать.
    std::function<void(Skin, std::filesystem::path)> onSave;

    std::function<void()> onChooseAnother;   ///< «Выбрать другое изображение»
    std::function<void()> onExit;            ///< «Выйти из мастера обложек»

private:
    /// За какую точку тянут: край держит вертикаль, вертикаль — горизонталь.
    enum class Grip { None, Top, Bottom, Line };

    void buildTree();
    wxl::Button overlayButton(std::wstring_view caption, void (SkinWizard::*handler)());

    /// Пересчитывает поверхность под размер окна и масштаб экрана.
    /// Возвращает true, если размер изменился.
    bool resizeSurface();

    void redraw();

    /// Точка под курсором и её вертикаль — или Grip::None.
    Grip gripAt(wxl::Point point, std::size_t& index) const;

    void chooseAnother();
    void exitWizard();

    void beginNaming();
    void finishNaming(bool save);

    wxl::Compositor compositor_;
    wxl::Nullable<wxl::Grid> root_ = nullptr;
    wxl::Nullable<wxl::Grid> surfaceHost_ = nullptr;   ///< несёт визуал снимка
    wxl::Nullable<wxl::SpriteVisual> visual_ = nullptr;
    std::vector<wxl::DrawingSurface> surface_;   ///< ноль или одна — как листы полосы

    /// Диалог имени. Свой оверлей, а не системное окно: он живёт поверх того
    /// же снимка, и «Отмена» возвращает ровно туда, где читатель был.
    wxl::Nullable<wxl::Border> namePanel_ = nullptr;
    wxl::Nullable<wxl::TextBox> nameBox_ = nullptr;

    std::filesystem::path image_;   ///< исходный снимок — источник будущей копии
    Microsoft::WRL::ComPtr<IWICFormatConverter> source_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> bitmap_;

    Skin skin_ = defaultSkin();                   ///< редактируемые точки
    std::array<float, Skin::kPoints> baseX_{};    ///< начальные места вертикалей

    Grip drag_ = Grip::None;
    std::size_t dragIndex_ = 0;

    /// Размер в DIP и масштаб экрана; поверхность — в их произведении.
    float width_ = 0.0f;
    float height_ = 0.0f;
    float scale_ = 1.0f;
};

}  // namespace bukvitsa::reader
