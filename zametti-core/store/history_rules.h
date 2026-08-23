// Что осталось от «правил истории»: две функции про БАЙТЫ ЗАМЕТКИ и отчёт
// чистки. Всё остальное разъехалось по своим хозяевам —
//
//   отбор (Rules, Step, Plan, planStep, planCompress) — у ZJournal: «писать ли
//     запись и кого погасить» это вопрос к набору записей;
//   сама чистка (файл) — у ZStorage::compressJournal.
//
// Эти же две функции остались здесь потому, что они не про журнал вовсе:
// «одна ли это версия заметки» — вопрос про ФОРМАТ ЗАМЕТКИ (в шапке живёт
// строка modified, она меняется на каждой записи и сама изменением не
// является). Журнал о формате не знает и знать не должен: у него безымянные
// байты.

#ifndef ZAMETTI_HISTORY_RULES_H
#define ZAMETTI_HISTORY_RULES_H

#include "journal.h"

#include <QByteArray>
#include <QString>
#include <QVector>

#include <functional>

namespace zametti {

// Одно ли это содержимое, если не считать строки modified в шапке.
//
// Штамп меняется на каждой записи и сам изменением заметки не является. Без
// этой оговорки «изменилось ли» отвечало бы «да» всегда, и каждая пауза в
// наборе давала бы на диске новую копию, а в истории — запись, отличающуюся
// одной цифрой в дате.
bool sameApartFromModified(const QByteArray& a, const QByteArray& b);

// Насколько две версии разошлись — в знаках, считая и дописанное, и стёртое.
//
// Меряется куском, который изменился: общее начало и общий хвост
// отбрасываются, остаётся то, что человек тронул. Разницы длин мало — два
// текста одной длины бывают разными целиком.
int changedChars(const QByteArray& a, const QByteArray& b);

namespace history {

// Прежнее имя свода правил: он переехал в журнал (ZJournal::Rules), а это
// псевдоним, чтобы сотня мест вызова не переписывалась ради переименования.
using Rules = ZJournal::Rules;

// Что сделала чистка — для люка и для отчётов.
struct Report {
    QString versionBefore;   // пусто — v0, «не чищен»
    QString versionAfter;
    int recordsBefore = 0;
    int recordsAfter = 0;
    int duplicates = 0;
    int merged = 0;
    bool rewritten = false;  // false — файл не тронут ни байтом
};

// Сама чистка переехала к хранилищу (ZStorage::compressJournal): она трогает
// файл, а файлы — дело хранилища.

}  // namespace history
}  // namespace zametti

#endif  // ZAMETTI_HISTORY_RULES_H
