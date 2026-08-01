// Конвейер вставки картинки: от файла до байтов, которые лягут в хранилище.
//
// Здесь живёт ТАБЛИЦА РЕШЕНИЙ — то место, где выбирается, что делать с
// конкретным файлом. Всё, что она использует, уже написано и проверено
// отдельно: границы входа (import_limits), чтение TIFF (tiff_reader),
// метаданные (exif), запись JXL (jxl_encoder), изменение размера (resample).
//
// Правила, которые таблица воплощает:
//
//   вход                          что делаем
//   ────────────────────────────  ──────────────────────────────────────────
//   JPEG, влезает в оба бюджета   лослесс-транскод в JXL с самопроверкой:
//                                 собрать обратно и сверить БАЙТ В БАЙТ; не
//                                 сошлось — положить исходник как есть
//   JXL, влезает                  как есть, байт в байт
//   WebP, влезает                 попытка перекодировать в JXL; заменяем
//                                 только при выигрыше, иначе как есть
//   PNG или битмап из буфера      проба на ~800 px: отношение меньше порога —
//                                 lossless JXL, иначе путь фото
//   всё прочее и всё, что не      путь фото: уменьшить до бюджета и сжать
//   влезает                       lossy JXL
//   анимация                      первый кадр, дальше по общим правилам
//
// ЧЕГО ЗДЕСЬ НЕТ И ПОЧЕМУ. Ни одного обращения к глобальным настройкам: все
// числа приходят параметром. Иначе конвейер нельзя было бы проверить, а
// проверять его надо на десятках сочетаний.

#pragma once

#include "import_limits.h"

#include <QByteArray>
#include <QImage>
#include <QString>

namespace zametti {

// Каким путём картинка попала в хранилище. Нужно и для отчёта recompress, и
// для проверок: «сжалось» само по себе ничего не говорит, а вот «пошло путём
// фото, хотя должно было транскодироваться» — говорит.
enum class Route {
    Refused,        // отказ: причина в ImportResult::refusal
    AsIs,           // байты источника легли без изменений
    TranscodedJpeg, // байт-точный транскод JPEG→JXL
    Lossless,       // lossless JXL
    Photo,          // путь фото: уменьшение и lossy JXL
};

const char* routeName(Route route);

struct ImportResult {
    Route route = Route::Refused;
    Refusal refusal = Refusal::None;
    QString message;        // человеку: почему отказ или что произошло

    QByteArray bytes;       // что класть в файл
    QString extension;      // "jxl" либо расширение источника, если AsIs
    Size size;              // размеры того, что легло
    int bitsPerSample = 8;

    // Отчёт: по нему видно, дорого ли обошлась картинка и почему.
    int encodes = 0;        // сколько раз звали энкодер
    int quality = 0;        // с каким качеством вышло (0 — не lossy)
    double ssimulacra2 = 0; // если арбитра звали; иначе 0
    qint64 sourceBytes = 0;

    bool ok() const { return route != Route::Refused; }
};

// Всё, что нужно знать о файле ДО решения. Заполняется по заголовку, без
// разжатия пикселей.
struct SourceInfo {
    QString format;         // как его назвал Qt: "jpeg", "png", "webp", "jxl"…
    Size size;
    int bitsPerSample = 8;
    qint64 fileBytes = 0;
    bool animated = false;
    bool hasAlpha = false;
    Refusal refusal = Refusal::None;
};

// Читает заголовок и проверяет границы. Пикселей не трогает.
SourceInfo probeSource(const QString& path, const ImportLimits& limits);

// Ввозит файл. limits — числа конфига; они же решают, что и как жать.
ImportResult importImage(const QString& path, const ImportLimits& limits);

// То же для картинки, которая пришла не файлом, а из буфера обмена. Формата у
// неё нет, метаданных тоже — только пиксели.
ImportResult importPixels(const QImage& image, const ImportLimits& limits);

}  // namespace zametti
