// ZImageCache — кэш картинок ПРИЛОЖЕНИЯ: заголовки, разжатые копии, бюджет.
//
// Один на программу и живёт у ZApp (решение владельца: кэш картинок — у
// приложения, к хранилищу отношения не имеет). Раньше это были поля вида
// (NoteView::imageCache_, imageOrder_, imageCacheBytes_) плюс два глобальных
// счётчика декодов — три уровня жизни в одном объекте (аудит refactor2 §1.3).
// Здесь кэш — объект со своими правилами:
//
//   * ЗАГОЛОВОК ЧИТАЕТСЯ СРАЗУ, ПИКСЕЛИ — ЛЕНИВО: info(path) даёт размеры и
//     справку без разжатия (место под фотографию считается по НАСТОЯЩИМ
//     размерам), pixels(path) разжимает по первому рисованию;
//   * БЮДЖЕТ МЯГКИЙ: вытесняются картинки чужих заметок с хвоста LRU;
//     картинки открытых заметок ЗАЩИЩЕНЫ — каждый вид называет свои
//     (protect(owner, keys)), и вытесняются они не бывают никогда (правило
//     владельца: на одну заметку кэша хватает всегда, пусть она одна и больше
//     бюджета);
//   * ЧТО НЕ ВЛЕЗЛО — РАМКА, и решение держится до перезапуска (plan):
//     считаем по заголовкам, раздаём место от самой мелкой к самой крупной;
//   * ПРОПАВШИЙ ФАЙЛ перепроверяется при каждом плане: рамка «файл не найден»
//     не имеет права застыть навсегда.
//
// Ключ — АБСОЛЮТНЫЙ путь: разрешить относительный от каталога заметки — дело
// вида, у него этот каталог есть. Бюджет и предел стороны приходят из
// настроек ЧЕРЕЗ ZApp (setLimits при старте и при перечитывании конфига), а не
// читаются самим кэшем.

#ifndef ZAMETTI_ZIMAGE_CACHE_H
#define ZAMETTI_ZIMAGE_CACHE_H

#include "image_metadata.h"

#include <QHash>
#include <QImage>
#include <QList>
#include <QSet>
#include <QSize>
#include <QString>

namespace zametti {

class ZImageCache {
public:
    enum class State {
        Pending,   // размеры знаем, пикселей ещё нет
        Shown,     // разжата, image непустой
        TooBig,    // Qt отказался разжимать: больше потолка
        Crowded,   // в кэш не влезла: заметка тяжелее бюджета
        Missing,   // файла нет: удалили руками или он ещё не приехал с синком
    };
    struct Entry {
        QImage image;
        QSize declared;      // размеры из заголовка файла; известны всегда
        // Всё, что показывает полоса сведений: имя, формат, вес, кадры, а
        // после разжатия — цвет и глубина. Живёт ЗДЕСЬ, а не в своём кэше
        // рядом: файл уже открыт и заголовок уже прочитан, а второй кэш дал бы
        // второй ответ на вопрос «что это за файл».
        ImageMetadata facts;
        qint64 bytes = 0;    // вес разжатой; у Pending, TooBig и Crowded ноль
        int limit = 0;       // предел стороны, которым ужимали: сменится — перечитаем
        State state = State::Pending;

        // Рамка вместо фотографии. Причины разные, а поведение одно: место
        // держим, пикселей не спрашиваем, надпись объясняет человеку, что не
        // так. Ссылка в заметке при этом неприкосновенна: вернётся файл или
        // поднимется потолок — вернётся и картинка.
        bool framed() const {
            return state == State::TooBig || state == State::Crowded || state == State::Missing;
        }
    };

    ZImageCache() = default;

    // Бюджет (МБ) и предел стороны разжатой копии (0 — выводится из экрана и
    // памяти машины, см. loadedImageSizeLimit). Задаёт ZApp из настроек.
    void setLimits(int budgetMb, int sideLimit);
    qint64 budgetBytes() const { return budgetBytes_; }
    int sideLimit() const;

    // Запись для абсолютного пути: размеры из заголовка, БЕЗ разжатия.
    // nullptr — файла нет и не было или он не картинка. Отсутствующий файл
    // получает запись Missing (рамка), чтобы место под него держалось.
    const Entry* info(const QString& absPath);
    // Заглянуть в запись, не трогая ни порядок, ни диск: nullptr — записи нет.
    const Entry* peek(const QString& absPath) const;
    // Пиксели: разжимает по первому спросу, если картинке отведено место.
    // nullptr — рисовать надо рамку (или записи нет).
    const QImage* pixels(const QString& absPath);

    // ЗАЩИЩЁННЫЕ КЛЮЧИ: картинки открытых заметок, по владельцу (виду). Они не
    // вытесняются. Вид называет их по одной, по мере спроса (protect), отпускает
    // все при полном пересчёте заметки (release) и в своём деструкторе (forget).
    void protect(const void* owner, const QString& key);
    void release(const void* owner);
    void forget(const void* owner) { release(owner); }
    const QSet<QString>& protectedBy(const void* owner) const;
    // Раздать место защищённым картинкам ЭТОГО владельца от мелкой к крупной:
    // что не влезло — Crowded (рамка), что больше потолка Qt — TooBig; пропавшие
    // файлы перепроверяются. Ни одного разжатия.
    void plan(const void* owner);

    // Телеметрия и наборы.
    qint64 bytes() const { return bytes_; }
    int count() const { return int(entries_.size()); }
    int shownCount() const;
    int framedCount() const;
    int decodes() const { return decodes_; }
    qint64 decodeMicros() const { return decodeMicros_; }
    void resetCounters();
    void clear();

    // Сколько займёт разжатая копия с этим пределом стороны.
    static qint64 decodedBytes(QSize declared, int limit);

protected:
    QHash<QString, Entry> entries_;
    QList<QString> order_;             // свежие в начале
    qint64 bytes_ = 0;
    qint64 budgetBytes_ = qint64(1024) * 1024 * 1024;
    int sideLimit_ = 0;
    QHash<const void*, QSet<QString>> protectedBy_;
    int decodes_ = 0;
    qint64 decodeMicros_ = 0;

    bool isProtected(const QString& key) const;
    void touch(const QString& key);
    void drop(const QString& key);
    // Вытесняет с хвоста, пока не уложится всё, что лежит, плюс need байт под
    // то, что добавляют; защищённые и keep не трогает.
    void trim(const QString& keep, qint64 need);
};

}  // namespace zametti

#endif  // ZAMETTI_ZIMAGE_CACHE_H
