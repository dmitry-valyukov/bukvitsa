// Точки входа, которыми части библиотеки пользуются друг у друга.
//
// Партиция, а не свободное объявление в .cpp: единицы реализации одного модуля
// друг друга не видят, поэтому то, что зовут из соседнего файла, обязано лежать
// в партиции. Первичный интерфейс её не переэкспортирует — здесь `wxl.xml`,
// а о ней публичный вид FB3 не знает и знать не должен.
export module bukvitsa.fb3:internal;

import wxl.xml;

import :description;

export namespace bukvitsa::fb3 {

/// Разбирает уже прочитанное дерево description.xml.
Description readDescription(const wxl::xml::node& root);

}  // namespace bukvitsa::fb3
