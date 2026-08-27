// Командный вид хранилища: `zametti store <команда> …`.
//
// Здесь только РАЗБОР И ПЕЧАТЬ: слово команды, ключи, коды возврата, тексты.
// Работа делается методами ZStorage — параллельной реализации «как бы того же
// самого» у утилиты нет и не будет.
//
// Отдельной программой (zametti-store) это было до 26.08.2026. Слияние —
// решение владельца: оба исполняемых файла тащили внутри всё завендоренное
// добро целиком (кодеки картинок, libsodium, zstd, blake3, highway), и после
// перехода на самодостаточные сборки дистрибутив выходил вдвое толще нужного.
// Заодно подкоманда получила то, чего у отдельной программы не было: широкий
// argv под Windows (путь с кириллицей) и подцепку к консоли родителя.

#ifndef ZAMETTI_STORE_CLI_H
#define ZAMETTI_STORE_CLI_H

#include "zstorage.h"

#include <QString>
#include <QStringList>

namespace zametti {

class StoreCli {
public:
    // args[0] — программа, args[1] — команда: слово `store` снял вызывающий.
    // Объект приложения Qt и платформа — забота вызывающего (app/main.cpp):
    // ядро зависит от QtGui, а дисплея утилите не нужно.
    explicit StoreCli(const QStringList& args) : args_(args) {}

    int run();

protected:
    // Справка в stderr и код 2 — она появляется как реакция на ошибку
    // употребления. Осознанный `--help` печатает её же в stdout с нулём.
    int usage() const;
    void printLines(const ZStorage::Report& report) const;
    // Замок хранилища одной дорогой; печатает отказ и отвечает, взяли ли.
    // busyHint — что дописать к «занято» (у thin своя приписка).
    bool takeLock(ZStorage& storage, const char* busyHint = "") const;
    // Адрес облака, названный ключами (--url/--to/--user).
    // Без облака, если не назван ни один адрес: тогда его знает remote.json.
    ZStorage::Config addressFromFlags() const;
    // Пароль с клавиатуры, БЕЗ эха. Разговор с терминалом — дело CLI, в ядре
    // ему места нет.
    static QString askPassword(const char* prompt);

    // Разбор пар «--ключ значение»; порядок свободный. false — употребление.
    bool parse();

    int cmdInit();
    int cmdSetRemote();
    int cmdSync();
    int cmdPushAll();
    int cmdRoot();
    int cmdNew();
    int cmdImport();
    int cmdArchive();
    int cmdRemove();
    int cmdResurrect();
    int cmdThin();
    int cmdHistory();
    int cmdRecompress();
    int cmdVerify();

    QStringList args_;
    QString command_;

    QString root_;
    QString from_;
    QString manifest_;
    QString parent_;
    QString positional_;
    QString positional2_;
    QString id_;
    QString maxSize_;
    QString quality_;
    QString url_;
    QString user_;
    QString to_;
    bool reset_ = false;
    bool pushOnly_ = false;
    bool allowMassDelete_ = false;
    bool keepAll_ = false;
    bool dryRun_ = false;
    bool restore_ = false;
};

}  // namespace zametti

#endif
