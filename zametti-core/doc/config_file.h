// ZConfigFile — МОДЕЛЬ конфига для редактора настроек внутри программы.
//
// MVC режима правки настроек (refactor3, решение владельца): модель — этот
// класс (файл config.json: путь, текст, чтение, запись, проверка), вид —
// JsonEditView (QPlainTextEdit с подсветкой JSON, в app), контроллер —
// SettingsController (в app). Модель держит ТЕКСТ, а не QTextDocument:
// рабочий документ и стек отмены сеанса принадлежат виду (как у режима
// исходника markdown), контроллер забирает текст вида при записи.
//
// Нет файла — при загрузке пишется шаблон (все параметры закомментированы,
// см. configTemplate в settings.h): открывать человеку пустоту и предлагать
// «наберите сами» нельзя. Запись атомарная (QSaveFile). Проверка —
// тем же путём, что и загрузчик настроек: срезать //-комментарии и висячие
// запятые, разобрать, сверить ключи со словарём умолчаний; она ничего не
// применяет — применяет окно, одним местом, что и при внешней правке файла.

#ifndef ZAMETTI_CONFIG_FILE_H
#define ZAMETTI_CONFIG_FILE_H

#include <QByteArray>
#include <QString>
#include <QStringList>

namespace zametti {

class ZConfigFile {
public:
    // Путь по умолчанию — configPath() (~/.config/zametti/config.json).
    explicit ZConfigFile(QString path = QString());

    // Прочитать файл; нет файла — сперва записать шаблон. false — не
    // читается/не пишется, причина в error.
    bool load(QString* error = nullptr);
    const QString& text() const { return text_; }
    void setText(const QString& text) { text_ = text; }
    // Отличается ли текст от того, что лежит на диске (последнее прочитанное
    // или записанное).
    bool dirty() const;
    // Записать текст атомарно. false — не записалось, причина в error; файл
    // при этом нетронут.
    bool save(QString* error = nullptr);

    // Итог проверки текста: разбирается ли (строка и колонка первой беды, с
    // единицы), и какие ключи не известны программе.
    struct Check {
        bool ok = true;
        int line = 0;
        int column = 0;
        QString error;
        QStringList unknownKeys;
    };
    static Check check(const QString& text);

protected:
    QString path_;
    QString text_;
    QByteArray onDisk_;
};

}  // namespace zametti

#endif  // ZAMETTI_CONFIG_FILE_H
