#pragma once
// Мастер обложек: читатель укладывает кривые по краям листа своего снимка.
//
// Мастер — не отдельный экран, а оверлей поверх полосы набора: под ним
// BookView рисует снимок и текущую читаемую страницу, изогнутую по
// редактируемым кривым (предпросмотр включает setPreview). Сам мастер рисует
// только сетку: у каждой из четырёх границ — верха и низа левого и правого
// листа — своя кривая из пяти точек, точки тянутся мышью в обе оси, а между
// верхней и нижней кривыми каждого листа идут интерполированные линии —
// подсказка, как лягут строки. Карта изгиба пересчитывается по отпусканию
// точки, а не на каждом движении: тянуть должно быть легко.
//
// Все координаты живут в долях ширины и высоты, поэтому смена размеров окна
// ничего не теряет. Диска здесь нет: снимок мастер только проверяет, а копию
// кладёт приложение — через свой рабочий поток.

#include <array>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "skins.h"

#include "DrawingSurface.h"
#include "Nullable.h"
#include "pch.h"

namespace bukvitsa::reader {

class SkinWizard {
public:
    explicit SkinWizard(const wxl::Compositor& compositor);

    /// Оверлей, который кладётся поверх полосы набора.
    const wxl::UIElement& root() const { return root_.value(); }

    /// Начинает новую обложку с этого снимка: точки на начальных местах, имя
    /// пустое. false — файл не раскодировался как картинка.
    bool openNew(std::filesystem::path image);

    /// Открывает существующую обложку на правку: её кривые, её снимок, её
    /// имя в диалоге сохранения. false — копия снимка не раскодировалась.
    bool openEdit(const Skin& skin);

    void show();
    void hide();
    bool isOpen() const { return open_; }

    /// Редактируемые кривые и снимок — то, что приложение отдаёт полосе как
    /// предпросмотр.
    const Skin& skin() const { return skin_; }
    const std::filesystem::path& imagePath() const { return image_; }

    /// Точку отпустили — кривые устоялись, пора пересчитать карту изгиба.
    std::function<void()> onCurvesChanged;

    /// «Сохранить», имя уже введено и не пустое. У правки старой обложки
    /// `image` заполнен — копия уже лежит в реестре; у новой пуст, и копию
    /// снимает приложение со второго аргумента.
    std::function<void(Skin, std::filesystem::path)> onSave;

    std::function<void()> onChooseAnother;   ///< «Выбрать другое изображение»
    std::function<void()> onExit;            ///< «Выйти из мастера обложек»

private:
    void buildTree();
    wxl::Button overlayButton(std::wstring_view caption, void (SkinWizard::*handler)());

    /// Кривая по номеру: две верхние, две нижние. Номер и есть память о том,
    /// какую точку тянут.
    EdgeCurve& curve(int index);
    const EdgeCurve& curve(int index) const;

    /// Пересчитывает поверхность сетки под размер окна и масштаб экрана.
    bool resizeSurface();

    void redraw();

    /// Точка под курсором: номер кривой и номер точки. false — мимо.
    bool gripAt(wxl::Point point, int& curveIndex, std::size_t& pointIndex) const;

    void chooseAnother();
    void exitWizard();

    void beginNaming();
    void finishNaming(bool save);

    wxl::Compositor compositor_;
    wxl::Nullable<wxl::Grid> root_ = nullptr;
    wxl::Nullable<wxl::Grid> surfaceHost_ = nullptr;   ///< несёт визуал сетки
    wxl::Nullable<wxl::SpriteVisual> visual_ = nullptr;
    std::vector<wxl::DrawingSurface> surface_;   ///< ноль или одна — как листы полосы

    /// Диалог имени. Свой оверлей, а не системное окно: он живёт поверх той
    /// же страницы, и «Отмена» возвращает ровно туда, где читатель был.
    wxl::Nullable<wxl::Border> namePanel_ = nullptr;
    wxl::Nullable<wxl::TextBox> nameBox_ = nullptr;

    std::filesystem::path image_;   ///< снимок обложки: выбранный файл или копия из реестра
    Skin skin_ = defaultSkin();     ///< редактируемые кривые (и имя с копией у правки)

    bool open_ = false;
    bool dragging_ = false;
    int dragCurve_ = 0;
    std::size_t dragPoint_ = 0;

    /// Размер в DIP и масштаб экрана; поверхность — в их произведении.
    float width_ = 0.0f;
    float height_ = 0.0f;
    float scale_ = 1.0f;
};

}  // namespace bukvitsa::reader
