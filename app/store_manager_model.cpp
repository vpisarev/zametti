// StoreManagerModel: вид окна хранилищ. Подробности — в заголовке.
//
// Файл уцелел в катастрофе 30.08.2026 (рабочая копия /tmp/smm.bak) и
// восстановлен с правками брифа того же дня: даты в строках фактов, полный
// адрес в переспросе стирания, «Create» только при пустом облаке, отказ
// относительному каталогу-облаку.

#include "store_manager_model.h"

#include <QDir>
#include <QFileInfo>

namespace zametti {

namespace {

using Known = ZStorageManager::Known;

// Объём — как его показывало окно с самого начала: мегабайты с одним знаком.
QString megabytes(qint64 bytes) {
    return QString::number(double(bytes) / (1024.0 * 1024.0), 'f', 1);
}

// Строка фактов — две строки на рамку (п.5 брифа): суммы, под ними последняя
// правка ISO-8601 с офсетом (правило времён проекта; сравнения — по UTC).
QString countsLine(const char* lead, int notes, int attachments, qint64 bytes,
                   const QDateTime& modified) {
    QString out = QStringLiteral("%1: %2 notes, %3 attachments, %4 MB")
                      .arg(QLatin1String(lead))
                      .arg(notes)
                      .arg(attachments)
                      .arg(megabytes(bytes));
    if (modified.isValid())
        out += QStringLiteral("\nmodified %1")
                   .arg(modified.toLocalTime().toString(Qt::ISODate));
    return out;
}

// Заголовок строки списка: имя корневой заметки, а нет его — имя папки. Пустая
// строка в списке хуже некрасивой.
QString rowTitle(const ZStorage::Config& e) {
    if (!e.name.isEmpty()) return e.name;
    const QString base = QFileInfo(e.root).fileName();
    return base.isEmpty() ? e.root : base;
}

}  // namespace

StoreManagerModel::StoreManagerModel(ZStorageManager& stores, const QString& openRoot)
    : stores_(stores), openRoot_(ZStorageManager::canonicalRoot(openRoot)) {
    // Выбор при открытии окна: открытое хранилище, а нет его — первая строка.
    // Окно без выбранной строки говорить не о чем.
    for (int i = 0; i < stores_.size(); ++i) {
        if (stores_.stores().at(i).root == openRoot_) {
            select(i);
            return;
        }
    }
    if (!stores_.isEmpty()) select(0);
}

QString StoreManagerModel::rootKey(int row) const {
    if (row < 0 || row >= stores_.size()) return {};
    return stores_.stores().at(row).root;
}

ZStorage::Config StoreManagerModel::rowConfig(int row) const {
    if (row < 0 || row >= stores_.size()) return {};
    return stores_.stores().at(row);
}

ZStorageManager::Facts StoreManagerModel::rowFacts(int row) const {
    const Draft d = drafts_->value(rootKey(row));
    // ПО ПОЛЮ ПУТИ, А НЕ ПО СТРОКЕ: человек мог набрать другую папку и ещё не
    // применить — показывать он обязан то, что набрано.
    const QString folder = d.folder.isEmpty() ? rowConfig(row).root : d.folder;
    if (folder.isEmpty()) return {};
    return stores_.facts(folder);
}

void StoreManagerModel::select(int row) {
    if (row < 0 || row >= stores_.size()) {
        selected_ = -1;
        return;
    }
    selected_ = row;
    const QString key = rootKey(row);
    if (drafts_->contains(key)) return;

    // Черновик заводится ОДИН РАЗ на строку и живёт до закрытия окна: человек
    // набрал адрес, ушёл посмотреть соседнюю строку, вернулся — набранное на
    // месте. Секреты в черновик не кладутся никогда; вместо них — заглушка,
    // если связка говорит, что запись есть.
    const ZStorage::Config e = rowConfig(row);
    Draft d;
    d.folder = e.root;
    // КАК ХРАНИТСЯ, ТАК И ПОКАЗЫВАЕТСЯ: разрезалки адреса не существует
    // (закон владельца). Каталог-облако — целиком в поле адреса.
    d.server = e.cloudUrl.isEmpty() ? e.cloudDir : e.cloudUrl;
    d.serverDir = e.cloudServerDir;
    d.login = e.cloudUser;
    drafts_->insert(key, d);
}

const StoreManagerModel::Draft& StoreManagerModel::draft() const {
    static const Draft kNone;
    const auto it = drafts_->constFind(rootKey(selected_));
    return it == drafts_->constEnd() ? kNone : *it;
}

StoreManagerModel::Draft& StoreManagerModel::draft() {
    return (*drafts_)[rootKey(selected_)];
}

// СКЛЕРОЗ ЛЕЧИТСЯ ЗДЕСЬ (закон владельца, 30.08.2026): всё набранное в полях
// адреса и логина переживает и закрытие окна, и выход из программы — даже
// когда связь не состоялась (опечатка, сеть, наша ошибка). Набранное уезжает
// в строку списка, а список — в state.json на выходе (пишет ZApp). Пароли на
// диск не идут никогда — их бережёт окно (связка, см. stashAll диалога).
void StoreManagerModel::stashDrafts() {
    for (auto it = drafts_->constBegin(); it != drafts_->constEnd(); ++it) {
        ZStorage::Config row = stores_.storeFor(it.key());
        if (row.root.isEmpty()) continue;   // строку успели забыть «−»
        ZStorage::Config typed;
        typed.setCloudAddress(it->server, it->serverDir, it->login);
        if (row.sameCloudAddress(typed)) continue;
        row.takeCloudAddress(typed);
        stores_.remember(row);
    }
}

void StoreManagerModel::adoptDrafts(QHash<QString, Draft>* drafts) {
    if (drafts == nullptr) return;
    drafts_ = drafts;
    // Выбор уже сделан конструктором, а черновик заводился в СВОЁМ хранилище:
    // перечитываем строку, теперь уже с сеансовыми.
    const int row = selected_;
    selected_ = -1;
    select(row);
}

bool StoreManagerModel::isOpenRow() const {
    return !openRoot_.isEmpty() && rootKey(selected_) == openRoot_;
}

void StoreManagerModel::setMessage(const QString& text, bool alarm) {
    message_ = text;
    messageAlarm_ = alarm;
}

void StoreManagerModel::noteSeen(const CloudSeen& seen) {
    const QString key = rootKey(selected_);
    if (key.isEmpty()) return;
    seen_.insert(key, seen);
}

const StoreManagerModel::CloudSeen& StoreManagerModel::seen() const {
    static const CloudSeen kNone;
    const auto it = seen_.constFind(rootKey(selected_));
    return it == seen_.constEnd() ? kNone : *it;
}

QString StoreManagerModel::cellCode() const {
    if (stores_.isEmpty() || selected_ < 0) return QStringLiteral("Zero");
    const Draft& d = draft();
    const ZStorageManager::Facts f = rowFacts(selected_);

    QString code;
    switch (f.kind) {
        case ZStorage::DirKind::Store:   code += QLatin1Char('V'); break;
        case ZStorage::DirKind::Empty:   code += QLatin1Char('E'); break;
        case ZStorage::DirKind::Missing: code += QLatin1Char('M'); break;
        case ZStorage::DirKind::Foreign: code += QLatin1Char('X'); break;
    }

    const ZStorage::Config cfg = cfgFromDraft();
    const bool named = cfg.hasCloudAddress();
    if (!named) {
        code += QStringLiteral("L-");
    } else {
        code += QLatin1Char('W');
        // Строчный хвостик — только у сервера с логином: каталогу-облаку
        // пароль не нужен вовсе, и «связываться нечем» к нему не относится.
        if (!cfg.cloudUrl.isEmpty()) {
            if (!d.serverPassword.isEmpty())         code += QLatin1Char('w');
            else if (f.serverPassword == Known::Yes) code += QLatin1Char('k');
            else                                     code += QLatin1Char('h');
        }
        switch (seen().state) {
            case CloudSeen::State::Empty: code += QLatin1Char('E'); break;
            case CloudSeen::State::Ours:  code += QLatin1Char('V'); break;
            case CloudSeen::State::Foreign:
            case CloudSeen::State::Incomplete: code += QLatin1Char('X'); break;
            default: code += QLatin1Char('?'); break;   // ещё не смотрели
        }
    }

    if (!d.encryptionPassword.isEmpty()) code += QLatin1Char('P');
    else if (f.key == Known::Yes)        code += QLatin1Char('K');
    else                                 code += QLatin1Char('N');

    if (!openRoot_.isEmpty() && rootKey(selected_) == openRoot_) code += QLatin1Char('o');
    return code;
}

StoreManagerModel::Snapshot StoreManagerModel::snapshot() const {
    Snapshot out;
    for (int i = 0; i < stores_.size(); ++i) {
        const ZStorage::Config e = stores_.stores().at(i);
        Row r;
        r.title = rowTitle(e);
        r.root = e.root;
        r.open = !openRoot_.isEmpty() && e.root == openRoot_;
        out.rows.append(r);
    }
    out.selected = selected_;
    out.message = Line{message_, messageAlarm_};

    // «+» и «закрыть» живы ВСЕГДА: окно не запирает человека внутри себя, и
    // завести первое хранилище должно быть можно из пустого списка (п.15).
    out.add = Button{true, QStringLiteral("+")};
    out.close = Button{true, QStringLiteral("Close")};
    out.remove = Button{false, QStringLiteral("−")};
    out.browse = Button{false, QStringLiteral("…")};
    out.check = Button{false, QStringLiteral("Check")};
    out.reset = Button{false, QStringLiteral("Reset cloud…")};
    out.open = Button{false, QStringLiteral("Open")};

    if (stores_.isEmpty() || selected_ < 0) {
        // ПУСТОЙ СПИСОК ГОВОРИТ СЛОВАМИ. Молчащее окно с погашенными кнопками
        // — беда G разбора: человек не понимает, сломано оно или так и надо.
        out.message = Line{QStringLiteral("No storages yet — press + to add one."), false};
        return out;
    }

    const Draft& d = draft();
    const ZStorageManager::Facts f = rowFacts(selected_);
    const bool isOpen = rootKey(selected_) == openRoot_ && !openRoot_.isEmpty();

    out.folder = d.folder;
    out.folderFrozen = isOpen;
    // Папка не нашлась (переехала, унесли) — путь горит красным, Browse жив:
    // человек указывает новое место, и строка переезжает за ним (сценарий 3
    // владельца, 30.08.2026).
    out.folderMissing = f.kind == ZStorage::DirKind::Missing;
    out.repeat = d.repeat;
    out.server.text = d.server;
    // ЕДИНСТВЕННАЯ ОСТАВЛЕННАЯ ПОДСКАЗКА (решение владельца: «подсказка —
    // всегда признак плохого дизайна»). Эта уцелела потому, что говорит не о
    // поведении программы, а о ФОРМЕ значения: угадать, что здесь ждут адрес
    // сервера, а не имя, неоткуда.
    out.server.placeholder = QStringLiteral("https://webdav.server/dav");
    out.serverDir.text = d.serverDir;
    out.login.text = d.login;

    // ЛОКАЛЬНАЯ ПАПКА ВМЕСТО ОБЛАКА — хак и недокументированная возможность
    // (решение владельца): обычному человеку она не нужна, а для наборов и
    // проб удобна. Логина, пароля сервера и серверной папки у каталога нет
    // вовсе — поля, которым нечего принимать, гаснут и говорят это сами.
    const bool folderCloud = !cfgFromDraft().cloudDir.isEmpty();
    const QString nothing = folderCloud ? QStringLiteral("<empty>") : QString();
    out.login.enabled = !folderCloud;
    out.login.placeholder = nothing;
    out.serverDir.enabled = !folderCloud;
    // ЖИВОЙ ДЕФОЛТ СЕРВЕРНОЙ ПАПКИ — ИМЯ ЛОКАЛЬНОЙ, СЕРЕНЬКИМ (решение
    // владельца, п.8 брифа): placeholder говорит, что ляжет в пустое поле,
    // ещё до того, как назван адрес облака. Показывается он ровно там, где
    // будет ПРИМЕНЁН (cfgFromDraft): у строки с прежним адресом пустое поле
    // значит «адрес уже полный», и серое там было бы враньём.
    out.serverDir.placeholder =
        folderCloud || rowConfig(selected_).hasCloudAddress()
            ? nothing
            : QFileInfo(d.folder.trimmed()).fileName();

    // ПОЛЯ ПАРОЛЕЙ ОДИНАКОВЫ — намеренно: у пароля сервера ровно те же три
    // состояния, что у пароля шифрования (набран, лежит в связке, нет нигде), и
    // человеку незачем держать в голове две разные грамматики.
    //
    // Глаз показывает НАСТОЯЩИЙ пароль, а он лежит отдельной записью связки.
    // Поэтому у поля шифрования кружочки говорят про КЛЮЧ, а глаз — про ПАРОЛЬ:
    // ключ без пароля бывает (пароль набрали на другом устройстве), и глазу
    // тогда показывать нечего.
    out.serverPassword.enabled = !folderCloud;
    out.serverPassword.text = d.serverPassword;
    out.serverPassword.stub = !d.serverPasswordTouched && d.serverPassword.isEmpty() &&
                              f.serverPassword == Known::Yes;
    out.serverPassword.eyeEnabled =
        !d.serverPassword.isEmpty() || f.serverPassword == Known::Yes;
    // Пароль сервера нужен только серверу: каталог-облако его не спрашивает.
    const bool wantsServerPassword = !cfgFromDraft().cloudUrl.isEmpty();
    if (folderCloud) {
        out.serverPassword.placeholder = nothing;
    } else if (wantsServerPassword && d.serverPassword.isEmpty() &&
               !out.serverPassword.stub) {
        out.serverPassword.placeholder = QStringLiteral("not on this device");
        out.serverPassword.placeholderAlarm = f.serverPassword == Known::No;
    }

    // Кружочки у пароля шифрования говорят про КЛЮЧ: он и есть то, чем облако
    // открывается. Сам пароль — отдельная запись, и он нужен лишь глазу.
    out.encryptionPassword.text = d.encryptionPassword;
    out.encryptionPassword.stub = !d.encryptionTouched && d.encryptionPassword.isEmpty() &&
                                  f.key == Known::Yes;
    out.encryptionPassword.eyeEnabled =
        !d.encryptionPassword.isEmpty() || f.encryptionPassword == Known::Yes;
    // ТРЕБОВАТЬ ПАРОЛЬ ТАМ, ГДЕ ОБЛАКА НЕТ, — шум: шифровать нечего и незачем.
    // Красное на пустом месте приучает не замечать красного вообще.
    const bool cloudNamed = cfgFromDraft().hasCloudAddress();
    if (cloudNamed && d.encryptionPassword.isEmpty() && !out.encryptionPassword.stub) {
        out.encryptionPassword.placeholder = QStringLiteral("not on this device");
        // КРАСНЫМ — только когда связка ТОЧНО сказала «нет». Красное требование
        // там, где никто не искал, — враньё (беда J разбора).
        out.encryptionPassword.placeholderAlarm = f.key == Known::No;
    }

    // ПОВТОР ПАРОЛЯ — ТОЛЬКО ТАМ, ГДЕ ОПЕЧАТКА НЕИСПРАВИМА. Пустое облако
    // запечатывается НАБРАННЫМ паролем: опечатались — и облако закрыто
    // навсегда, потому что проверить его не обо что. Где конверт уже есть,
    // повтор не нужен вовсе: неверный пароль там просто не подойдёт.
    const CloudSeen& seenNow = seen();
    const QString addressNow = cfgFromDraft().cloudAddressText();
    out.repeatVisible = seenNow.address == addressNow &&
                        seenNow.state == CloudSeen::State::Empty &&
                        !d.encryptionPassword.isEmpty();

    // --- буквы клетки -------------------------------------------------------
    // ОБЛАКО НАЗВАНО — ЭТО ПРО ПОЛЕ АДРЕСА. Каталог-облако вводится в то же
    // поле, что и webdav-сервер (различает их setCloudAddress по «http://»);
    // «Cloud dir» — сегмент ВНУТРИ сервера, и сам по себе он облака не
    // называет.
    const bool named = !d.server.trimmed().isEmpty();
    const bool isStore = f.kind == ZStorage::DirKind::Store;
    const bool isEmptyDir = f.kind == ZStorage::DirKind::Empty;
    const bool haveEncryption =
        !d.encryptionPassword.isEmpty() || out.encryptionPassword.stub;
    // «Связываться нечем» — только когда назван СЕРВЕР, пароля в поле нет и
    // связка ТОЧНО сказала, что его нет. Неизвестность не гасит: пусть попытка
    // скажет словами, чем кнопка промолчит.
    const bool nothingToConnectWith =
        wantsServerPassword && d.serverPassword.isEmpty() && !out.serverPassword.stub &&
        f.serverPassword != Known::Unknown;
    // Относительный каталог-облако — не адрес: ядро его отвергнет, а кнопки
    // честно гаснут и снизу сказано почему (урок 30.08.2026).
    const bool relativeCloudDir =
        folderCloud && QDir::isRelativePath(cfgFromDraft().cloudDir);

    // --- кнопки (таблица A разбора) -----------------------------------------
    out.remove.enabled = true;   // «−» жив всегда, и для открытого (п.14)
    out.browse.enabled = !isOpen;   // папка открытого под замком
    out.check.enabled = named && !relativeCloudDir &&
                        f.kind != ZStorage::DirKind::Foreign && !nothingToConnectWith;
    // «Reset cloud» ЖИВА ВСЕГДА, КОГДА ЕСТЬ ЧТО ПРОВЕРЯТЬ — то же условие, что
    // у Check, плюс «папка и правда хранилище». Пароль шифрования ей НЕ нужен
    // (уточнение владельца, 30.08.2026: новая машина, хранилище с флешки,
    // пароль забыт — стирание и сброс обязаны работать).
    //
    // Прежде она требовала, чтобы человек СНАЧАЛА нажал Check: связь надо
    // доказать, стирать вслепую нельзя. Правило верное, требование — нет
    // (владелец, 29.08.2026: «пароль от сервера уже есть, почему я не могу
    // нажать Reset cloud, check должен выполниться сам»). Проверку делает сам
    // жест, а переспрос показывается по её итогу — см. resetPressed.
    out.reset.enabled = isStore && out.check.enabled;
    // «Открыть» не запрещается зря (поправка владельца): открытое хранилище
    // просто закрывает окно, а пустая папка заводит новое.
    out.open.enabled = isStore || (isEmptyDir && !relativeCloudDir &&
                                   (!named || haveEncryption));
    // «Create» — только когда создаётся НОВОЕ: папка пуста И облако пусто или
    // не названо (п.2 брифа). Пустая папка при живом облаке — скачивание
    // существующего, это «Open».
    if (isEmptyDir &&
        (!named || (seenNow.address == addressNow &&
                    seenNow.state == CloudSeen::State::Empty)))
        out.open.label = QStringLiteral("Create");

    // --- строка «Local:» ----------------------------------------------------
    switch (f.kind) {
        case ZStorage::DirKind::Store:
            out.local = Line{countsLine("Local", f.stats.notes, f.stats.attachments,
                                        f.stats.bytes, f.stats.lastModified),
                             false};
            break;
        case ZStorage::DirKind::Empty:
            out.local = Line{QStringLiteral("Local: empty folder"), false};
            break;
        case ZStorage::DirKind::Missing:
            out.local = Line{QStringLiteral("Local: missing directory"), true};
            break;
        case ZStorage::DirKind::Foreign:
            // Формулировка владельца (п.4 брифа): супер-компактно, в две
            // строки рамки укладывается с запасом.
            out.local = Line{QStringLiteral("Local: not a valid storage nor empty dir"),
                             true};
            break;
    }

    // --- строка «Cloud:» ----------------------------------------------------
    const CloudSeen& s = seen();
    const QString address = cfgFromDraft().cloudAddressText();
    if (!named) {
        out.cloud = Line{QStringLiteral("Cloud: not set"), false};
    } else if (relativeCloudDir) {
        out.cloud = Line{QStringLiteral("Cloud: the folder must be an absolute path"),
                         true};
    } else if (s.address != address || s.state == CloudSeen::State::NotChecked) {
        out.cloud = Line{QStringLiteral("Cloud: not checked"), false};
    } else {
        switch (s.state) {
            case CloudSeen::State::Empty:
                // ПУСТОЕ ОБЛАКО — НЕ БЕДА, а законный старт: сюда и зальёмся
                // (поправка владельца). Поэтому обычным шрифтом.
                out.cloud = Line{QStringLiteral("Cloud: empty"), false};
                break;
            case CloudSeen::State::Ours:
                out.cloud = Line{countsLine("Cloud", s.notes, s.attachments, s.bytes,
                                            s.lastModified),
                                 false};
                break;
            case CloudSeen::State::Foreign:
                out.cloud = Line{QStringLiteral("Cloud: another storage"), true};
                break;
            case CloudSeen::State::Incomplete:
                out.cloud = Line{QStringLiteral("Cloud: incomplete — no keyfile"), true};
                break;
            case CloudSeen::State::NoAnswer:
                out.cloud = Line{QStringLiteral("Cloud: no answer"), true};
                break;
            case CloudSeen::State::LoginRefused:
                out.cloud = Line{QStringLiteral("Cloud: login refused"), true};
                break;
            case CloudSeen::State::WrongPassword:
                // СТРОКА ФАКТОВ — ПРО ОБЛАКО, А НЕ ПРО ИСХОД ПОПЫТКИ. Неверный
                // пароль — это событие, и говорит о нём сообщение; облако же в
                // этот миг известно: оно наше, запечатанное, и в нём столько-то
                // заметок. Прежде обе строки говорили одно и то же слово в
                // слово (жалоба владельца 30.08.2026).
                out.cloud = s.notes > 0 || s.attachments > 0
                                ? Line{countsLine("Cloud", s.notes, s.attachments,
                                                  s.bytes, s.lastModified),
                                       false}
                                : Line{QStringLiteral("Cloud: sealed"), false};
                break;
            case CloudSeen::State::NotChecked:
                break;   // разобрано выше
        }
    }

    // --- чего не хватает, сказанное словами ---------------------------------
    // Погашенная кнопка обязана объяснять себя: «connect нечем» и «пароля нет»
    // — беды N и P разбора, где совет указывал на погашенную кнопку или его не
    // было вовсе.
    if (out.message.text.isEmpty()) {
        if (relativeCloudDir)
            out.message = Line{QStringLiteral("The cloud folder must be an absolute path."),
                               false};
        else if (nothingToConnectWith)
            out.message = Line{QStringLiteral("Enter the server password."), false};
        else if (isEmptyDir && named && !haveEncryption)
            out.message = Line{QStringLiteral("Enter the encryption password."), false};
        else if (isStore && seenNow.address == addressNow &&
                 seenNow.state == CloudSeen::State::Empty && !haveEncryption)
            // Хранилище против пустого облака: следующий шаг — запечатать его
            // паролем, и об этом сказано словами, а не погашенной кнопкой.
            out.message = Line{QStringLiteral("Enter the encryption password twice — it "
                                              "will seal the cloud."),
                               false};
    }
    return out;
}


// --- ЖЕСТЫ ------------------------------------------------------------------

void StoreManagerModel::edit(FieldId field, const QString& text) {
    if (selected_ < 0) return;
    Draft& d = draft();
    switch (field) {
        case FieldId::Folder: d.folder = text; break;
        case FieldId::Server: d.server = text; break;
        case FieldId::ServerDir: d.serverDir = text; break;
        case FieldId::Login: d.login = text; break;
        // ПЕРВОЕ ЖЕ НАЖАТИЕ КЛАВИШИ СТИРАЕТ ЗАГЛУШКУ: дальше поле обычное, и
        // кружочки связки больше ни при чём. Ровно так ведут себя браузеры с
        // сохранённым паролем — этот язык человек уже знает.
        case FieldId::ServerPassword:
            d.serverPassword = text;
            d.serverPasswordTouched = true;
            break;
        case FieldId::EncryptionPassword:
            d.encryptionPassword = text;
            d.encryptionTouched = true;
            break;
        case FieldId::Repeat: d.repeat = text; break;
    }
    // Всякая правка — новый разговор: прежнее сообщение к нему не относится.
    message_.clear();
    messageAlarm_ = false;
}

ZStorage::Config StoreManagerModel::cfgFromDraft() const {
    ZStorage::Config cfg;
    cfg.root = draft().folder;
    // ЖИВОЙ ДЕФОЛТ СЕРВЕРНОЙ ПАПКИ — ИМЯ ЛОКАЛЬНОЙ (п.8 брифа). Placeholder
    // обещает её серым, значит В РАБОТУ обязано уезжать то же самое: без
    // подстановки Check уходил в КОРЕНЬ провайдера, где живёт всё подряд
    // (найдено живой пробой владельца 30.08 — «404» вместо папки). Дефолт —
    // только ПЕРВОМУ подключению строки: прежней записи (например, полному
    // адресу из CLI в cloudUrl) дописывать имя папки нельзя.
    QString serverDir = draft().serverDir.trimmed();
    if (serverDir.isEmpty() && !rowConfig(selected_).hasCloudAddress())
        serverDir = QFileInfo(draft().folder.trimmed()).fileName();
    cfg.setCloudAddress(draft().server, serverDir, draft().login);
    return cfg;
}

StoreManagerModel::Job StoreManagerModel::jobFor(Job::Kind kind) const {
    Job job;
    job.kind = kind;
    job.root = draft().folder;
    job.cfg = cfgFromDraft();
    // Запечатать пустое облако можно только пройдя повтор: заказчик жеста
    // (checkPressed/answered) уже прогнал sealingNeedsRepeat, здесь лишь
    // выписывается пропуск.
    job.sealEmpty = snapshot().repeatVisible && !draft().repeat.isEmpty() &&
                    draft().repeat == draft().encryptionPassword;
    job.serverPassword = draft().serverPassword;
    job.serverPasswordFromKeyring =
        draft().serverPassword.isEmpty() && !draft().serverPasswordTouched;
    job.encryptionPassword = draft().encryptionPassword;
    job.encryptionFromKeyring =
        draft().encryptionPassword.isEmpty() && !draft().encryptionTouched;
    return job;
}

StoreManagerModel::Reaction StoreManagerModel::addFolder(const QString& dir) {
    Reaction out;
    const QString key = ZStorageManager::canonicalRoot(dir);
    if (key.isEmpty()) return out;

    // Уже в списке — перескок, а не дубль. Молчать нельзя: человек нажал «+» и
    // вправе понять, почему строк не прибавилось.
    for (int i = 0; i < stores_.size(); ++i) {
        if (stores_.stores().at(i).root != key) continue;
        select(i);
        setMessage(QStringLiteral("Already in the list."));
        return out;
    }

    // ЧУЖАЯ ПАПКА СТРОКИ НЕ ПОЛУЧАЕТ. Строка без хранилища и без права его
    // завести — обманка: она выглядит как остальные и не умеет ничего.
    //
    // СООБЩЕНИЕ — ПРО ЖЕСТ, А НЕ ПРО ФАКТ. Строкой «Local: not a valid
    // storage…» здесь говорить нельзя: ровно это же слово в слово стоит в
    // строке фактов, когда выбранная строка тоже чужая папка, — и человек
    // получает две одинаковые красные строки. Обезьяна (zametti-bench
    // store-monkey) нашла этот случай на 28-м жесте первого круга.
    if (stores_.facts(key).kind == ZStorage::DirKind::Foreign) {
        setMessage(QStringLiteral("Not a zametti storage — nothing added."), true);
        return out;
    }

    ZStorage::Config row;
    row.root = key;
    stores_.remember(row);
    for (int i = 0; i < stores_.size(); ++i)
        if (stores_.stores().at(i).root == key) select(i);
    return out;
}

StoreManagerModel::Reaction StoreManagerModel::forgetPressed() {
    Reaction out;
    if (selected_ < 0) return out;
    const bool isOpen = rootKey(selected_) == openRoot_ && !openRoot_.isEmpty();
    out.question.kind = Question::Kind::Forget;
    out.question.text = QStringLiteral("Remove \"%1\" from the list?")
                            .arg(snapshot().rows.at(selected_).title);
    // Цена называется прямо: строка уходит, файлы и облако остаются.
    out.question.detail = isOpen
        ? QStringLiteral("Files stay on disk. This storage is open and will be closed.")
        : QStringLiteral("Files stay on disk.");
    out.question.choices = {QStringLiteral("Remove"), QStringLiteral("Cancel")};
    return out;
}

// Шаг «свежести» перед работой, которая ЗАПЕЧАТЫВАЕТ пустое облако. Ложь —
// работу не заказываем, человеку сказано, чего ждём.
bool StoreManagerModel::sealingNeedsRepeat() {
    const Snapshot snap = snapshot();
    if (!snap.repeatVisible) return false;
    const Draft& d = draft();
    if (d.repeat.isEmpty()) {
        setMessage(QStringLiteral("Repeat the password to seal it."));
        return true;
    }
    if (d.repeat != d.encryptionPassword) {
        setMessage(QStringLiteral("Passwords do not match."), true);
        return true;
    }
    return false;
}

StoreManagerModel::Reaction StoreManagerModel::checkPressed() {
    Reaction out;
    if (!snapshot().check.enabled) return out;
    if (sealingNeedsRepeat()) return out;
    out.job = jobFor(Job::Kind::Check);
    return out;
}

StoreManagerModel::Reaction StoreManagerModel::openPressed() {
    Reaction out;
    const Snapshot snap = snapshot();
    if (!snap.open.enabled) return out;

    const ZStorageManager::Facts f = rowFacts(selected_);
    if (f.kind == ZStorage::DirKind::Store) {
        // НАБРАННОЕ ОБЛАКО ПРИМЕНЯЕТСЯ ИМЕННО ЗДЕСЬ (живая проба владельца,
        // 30.08.2026: ввёл адрес и оба пароля, нажал Open — «синхронизация не
        // запускается, кнопка задизаблена». Запечатывал только второй Check,
        // а Open молча переключался, не записав адрес). Open — это «сделай
        // как набрано и открой»: есть несохранённая облачная настройка —
        // сперва та же работа, что у Check, открытие — её итогом.
        const ZStorage::Config typed = cfgFromDraft();
        const ZStorage::Config row = rowConfig(selected_);
        const bool cloudEdited = !typed.sameCloudAddress(row);
        const bool pendingSetup =
            typed.hasCloudAddress() &&
            (cloudEdited || !draft().encryptionPassword.isEmpty());
        if (pendingSetup && snap.check.enabled) {
            if (sealingNeedsRepeat()) return out;
            openAfterCheck_ = true;
            out.job = jobFor(Job::Kind::Check);
            return out;
        }
        // ПЕРЕЕЗД СТРОКИ (беда I матрицы; сценарий 3 владельца): папку
        // выбрали заново (Browse) — строка переезжает за ней вместе с
        // облаком, черновиком и памятью «что видели», а не оставляет сироту
        // со старым путём рядом с новой.
        const QString oldKey = rootKey(selected_);
        const QString newKey = ZStorageManager::canonicalRoot(draft().folder);
        const QString folder = draft().folder;
        if (!newKey.isEmpty() && newKey != oldKey) {
            ZStorage::Config moved = typed;
            moved.root = newKey;
            moved.name = row.name;
            stores_.forget(oldKey);
            stores_.remember(moved);
            drafts_->insert(newKey, drafts_->take(oldKey));
            if (seen_.contains(oldKey)) seen_.insert(newKey, seen_.take(oldKey));
            for (int i = 0; i < stores_.size(); ++i)
                if (stores_.stores().at(i).root == newKey) selected_ = i;
        }
        // Настраивать нечего — просто закрыть окно с переключением (поправка
        // владельца): жест не запрещается и на уже открытом.
        out.switchToRoot = folder;
        out.close = true;
        return out;
    }
    // Пустая папка: заводим — и переспрашиваем, потому что это создание.
    out.question.kind = Question::Kind::Create;
    out.question.text = QStringLiteral("Create a new storage in \"%1\"?").arg(draft().folder);
    out.question.choices = {QStringLiteral("Create"), QStringLiteral("Cancel")};
    return out;
}

StoreManagerModel::Reaction StoreManagerModel::resetPressed() {
    Reaction out;
    const Snapshot snap = snapshot();
    if (!snap.reset.enabled) return out;

    // СВЯЗЬ ПРОВЕРЯЕТ ПРОГРАММА, А НЕ ЧЕЛОВЕК. Не ходили по этому адресу —
    // сходим сейчас, и переспрос покажем по итогу: спрашивать «стереть?» про
    // облако, которого мы не видели, нельзя.
    const CloudSeen& s = seen();
    const QString address = cfgFromDraft().cloudAddressText();
    if (s.address != address || s.state == CloudSeen::State::NotChecked) {
        askResetAfterCheck_ = true;
        out.job = jobFor(Job::Kind::Check);
        // Разведка РАДИ СТИРАНИЯ — парольно-слепая: в поле может стоять
        // НОВЫЙ пароль (человек пришёл его менять), и разворачивать им
        // СТАРЫЙ конверт значило бы упереться в «wrong password» (живой
        // прогон 30.08.2026). Сводка и манифест открыты; дорогам Reset
        // пароль шифрования не нужен по построению.
        out.job.encryptionPassword.clear();
        out.job.encryptionFromKeyring = false;
        out.job.sealEmpty = false;
        return out;
    }
    if (s.state == CloudSeen::State::Empty) {
        // Стирать нечего — но уйти совсем человек вправе, и другой дороги
        // забыть адрес у него нет.
        out.question.kind = Question::Kind::ResetCloud;
        out.question.text =
            QStringLiteral("The cloud storage at %1 is empty — there is nothing to erase.")
                .arg(address);
        out.question.choices = {QStringLiteral("Disconnect"), QStringLiteral("Cancel")};
        return out;
    }

    out.question.kind = Question::Kind::ResetCloud;
    const ZStorageManager::Facts f = rowFacts(selected_);
    const bool keyAtHand = f.key == ZStorageManager::Known::Yes;

    // ПЕРЕСПРОС НАЗЫВАЕТ ПОЛНЫЙ АДРЕС, а не имя (урок 30.08.2026: переспрос,
    // называвший имя хранилища, не показал, ЧТО именно будет стёрто).
    if (keyAtHand) {
        // КЛЮЧ ПОД РУКОЙ — СТИРАТЬ НЕЧЕГО. Пароль меняется одним конвертом
        // (Keyfile::rewrap, блобы не трогаются), и предлагать разрушение там,
        // где оно не нужно, — значит пугать зря.
        out.question.text =
            QStringLiteral("Change the encryption password for \"%1\"?")
                .arg(snap.rows.at(selected_).title);
        out.question.detail =
            QStringLiteral("Erase and disconnect would erase %1.").arg(address);
        out.question.choices = {QStringLiteral("Change password"),
                                QStringLiteral("Erase and disconnect"),
                                QStringLiteral("Cancel")};
    } else {
        out.question.text =
            QStringLiteral("The cloud storage at %1 will be erased. Proceed?").arg(address);
        out.question.choices = {QStringLiteral("Erase and reset password"),
                                QStringLiteral("Erase and disconnect"),
                                QStringLiteral("Cancel")};
    }
    return out;
}

StoreManagerModel::Reaction StoreManagerModel::answered(Question::Kind kind, int choice) {
    Reaction out;
    if (selected_ < 0) return out;
    switch (kind) {
        case Question::Kind::Forget: {
            if (choice != 0) return out;
            const QString gone = rootKey(selected_);
            const bool wasOpen = gone == openRoot_ && !openRoot_.isEmpty();
            stores_.forget(gone);
            drafts_->remove(gone);
            seen_.remove(gone);
            // ОПУСТЕВШИЙ СПИСОК НЕ МОЛЧИТ, и поля не остаются от ушедшей
            // строки: снимок пересчитается с selected_ = -1 (беда G разбора).
            select(stores_.isEmpty() ? -1 : 0);
            if (wasOpen) out.close = true;   // открытое отцепляется немедленно
            return out;
        }
        case Question::Kind::Create:
            if (choice != 0) return out;
            // Та же осторожность, что и у Check: создание с пустым облаком
            // тоже запечатывает его набранным паролем.
            if (sealingNeedsRepeat()) return out;
            out.job = jobFor(Job::Kind::Create);
            return out;
        case Question::Kind::ResetCloud: {
            // У ПУСТОГО ОБЛАКА ДОРОГА ОДНА — отвязка, и она стоит первой.
            if (seen().state == CloudSeen::State::Empty) {
                if (choice == 0) out.job = jobFor(Job::Kind::EraseAndDisconnect);
                return out;
            }
            const ZStorageManager::Facts f = rowFacts(selected_);
            const bool keyAtHand = f.key == ZStorageManager::Known::Yes;
            if (choice == 0) {
                if (keyAtHand) {
                    // Новый пароль — набранный: заглушка связки паролем не
                    // считается. Повтор не нужен — ключ остаётся в связке, и
                    // опечатка лечится второй сменой.
                    if (draft().encryptionPassword.isEmpty()) {
                        setMessage(QStringLiteral("Enter the new encryption password."));
                        return out;
                    }
                    out.job = jobFor(Job::Kind::ChangePassword);
                } else {
                    // Стереть и запечатать заново может только НАБРАННЫЙ новый
                    // пароль (с повтором — облако станет пустым и запечатается
                    // им); просить его надо ДО стирания, не после.
                    if (draft().encryptionPassword.isEmpty()) {
                        setMessage(QStringLiteral(
                            "Enter the new encryption password twice — it will seal "
                            "the cloud."));
                        return out;
                    }
                    if (draft().repeat.isEmpty()) {
                        setMessage(QStringLiteral("Repeat the password to seal it."));
                        return out;
                    }
                    if (draft().repeat != draft().encryptionPassword) {
                        setMessage(QStringLiteral("Passwords do not match."), true);
                        return out;
                    }
                    out.job = jobFor(Job::Kind::EraseAndReseed);
                }
            } else if (choice == 1) {
                out.job = jobFor(Job::Kind::EraseAndDisconnect);
            }
            return out;
        }
        case Question::Kind::None:
            return out;
    }
    return out;
}

StoreManagerModel::Reaction StoreManagerModel::jobFinished(Job::Kind kind,
                                                           const Outcome& outcome) {
    Reaction next;
    if (outcome.seen.state != CloudSeen::State::NotChecked) noteSeen(outcome.seen);
    setMessage(outcome.message, outcome.alarm);
    // Проверка заказывалась ради переспроса или открытия — доделаем теперь,
    // когда есть что сказать про облако. Не удалась — намерение снимается
    // молча: объяснение уже в статусе.
    const bool wanted = askResetAfterCheck_ && kind == Job::Kind::Check;
    askResetAfterCheck_ = false;
    const bool wantedOpen = openAfterCheck_ && kind == Job::Kind::Check;
    openAfterCheck_ = false;
    if (!outcome.ok) return next;

    // Удача меняет ФАКТЫ строки: завелось хранилище, уехал конверт, лёг секрет.
    // Пересчитать их обязан кто-то один — вот он.
    const QString key = ZStorageManager::canonicalRoot(draft().folder);
    stores_.refresh(key);
    switch (kind) {
        case Job::Kind::Create:
        case Job::Kind::Check:
        case Job::Kind::EraseAndReseed:
        case Job::Kind::ChangePassword: {
            // Адрес ложится в строку: он больше не черновик, а факт.
            const QString oldKey = rootKey(selected_);
            ZStorage::Config row = cfgFromDraft();
            row.root = key;
            row.name = stores_.storeFor(key).name;
            if (row.name.isEmpty()) row.name = stores_.storeFor(oldKey).name;
            stores_.remember(row);
            // ПЕРЕЕЗД И ЗДЕСЬ (сценарий 4 владельца: на строке пропавшей
            // папки выбрали новую пустую и создали заново из облака) —
            // строка-сирота со старым путём не остаётся.
            if (!oldKey.isEmpty() && oldKey != key) {
                stores_.forget(oldKey);
                drafts_->insert(key, drafts_->take(oldKey));
                if (seen_.contains(oldKey)) seen_.insert(key, seen_.take(oldKey));
                for (int i = 0; i < stores_.size(); ++i)
                    if (stores_.stores().at(i).root == key) selected_ = i;
            }
            break;
        }
        case Job::Kind::EraseAndDisconnect: {
            // Отвязка — единственная дорога, стирающая адрес из строки: пустое
            // облако прежнего не затирает, и иначе забыть его было бы нечем.
            stores_.forget(key);
            ZStorage::Config bare;
            bare.root = key;
            stores_.remember(bare);
            Draft& d = draft();
            d.server.clear();
            d.serverDir.clear();
            d.login.clear();
            d.serverPassword.clear();
            d.serverPasswordTouched = true;   // облака нет — и кружочкам взяться неоткуда
            seen_.remove(key);
            break;
        }
        case Job::Kind::None:
            break;
    }
    if (wanted) next = resetPressed();
    if (wantedOpen) {
        // Открыть можно, когда настройка ДОДЕЛАНА. Единственный недоделанный
        // исход удачного Check — «облако пустое, пароль набран, повтора ещё
        // не было»: запечатывание требует повтора (свежесть), и открытие
        // подождёт следующего Open. Сказано словами тем же местом.
        if (seen().state == CloudSeen::State::Empty &&
            !draft().encryptionPassword.isEmpty()) {
            sealingNeedsRepeat();
        } else {
            next.switchToRoot = draft().folder;
            next.close = true;
        }
    }
    // Пароли в черновике после удачи не нужны: они уехали в связку, а держать
    // их на экране лишнюю минуту незачем.
    if (kind != Job::Kind::Check) {
        Draft& d = draft();
        // Пароли уехали в связку — в черновике им делать нечего, и «тронуто»
        // снимается: пусть поля снова показывают кружочки, если связка их
        // приняла. Показ выведется сам, запоминать его нельзя.
        d.encryptionPassword.clear();
        d.repeat.clear();
        d.encryptionTouched = false;
        d.serverPassword.clear();
        d.serverPasswordTouched = false;
    }
    return next;
}

}  // namespace zametti
