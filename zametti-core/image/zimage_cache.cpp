#include "zimage_cache.h"

#include "image_read.h"
#include "settings.h"

#include <QElapsedTimer>
#include <QFileInfo>
#include <QImageReader>

#include <algorithm>
#include <utility>
#include <vector>

namespace zametti {

void ZImageCache::setLimits(int budgetMb, int sideLimit) {
    budgetBytes_ = qint64(qMax(8, budgetMb)) * 1024 * 1024;
    sideLimit_ = qMax(0, sideLimit);
}

int ZImageCache::sideLimit() const {
    // Явный предел из настроек; ноль — выведенный из экрана и памяти машины.
    return sideLimit_ > 0 ? sideLimit_ : loadedImageSizeLimit();
}

qint64 ZImageCache::decodedBytes(QSize declared, int limit) {
    QSize shown = declared;
    if (limit > 0 && (shown.width() > limit || shown.height() > limit)) {
        shown = shown.scaled(limit, limit, Qt::KeepAspectRatio);
        shown.setWidth(qMax(1, shown.width()));
        shown.setHeight(qMax(1, shown.height()));
    }
    // Четыре байта на точку: столько занимает разжатая копия в памяти.
    return qint64(shown.width()) * shown.height() * 4;
}

bool ZImageCache::isProtected(const QString& key) const {
    for (const QSet<QString>& keys : protectedBy_)
        if (keys.contains(key)) return true;
    return false;
}

void ZImageCache::touch(const QString& key) {
    const qsizetype at = order_.indexOf(key);
    if (at <= 0) return;   // уже свежайшая или её нет
    order_.move(at, 0);
}

void ZImageCache::drop(const QString& key) {
    const auto it = entries_.constFind(key);
    if (it != entries_.constEnd()) bytes_ -= it->bytes;
    entries_.remove(key);
    order_.removeAll(key);
}

// Порядок именно такой: сначала добавили, потом убираем лишнее. Спрашивать
// «сколько она весит» до разжатия негде — вес узнаётся из самого декода.
void ZImageCache::trim(const QString& keep, qint64 need) {
    for (qsizetype i = order_.size() - 1; i >= 0 && bytes_ + need > budgetBytes_; --i) {
        const QString key = order_.at(i);
        if (key == keep || isProtected(key)) continue;
        const auto it = entries_.constFind(key);
        if (it != entries_.constEnd()) bytes_ -= it->bytes;
        entries_.remove(key);
        order_.removeAt(i);
    }
}

void ZImageCache::protect(const void* owner, const QString& key) {
    if (owner == nullptr || key.isEmpty()) return;
    protectedBy_[owner].insert(key);
}

void ZImageCache::release(const void* owner) { protectedBy_.remove(owner); }

const ZImageCache::Record* ZImageCache::info(const QString& abs) {
    if (abs.isEmpty()) return nullptr;
    const int limit = sideLimit();
    auto it = entries_.find(abs);
    if (it != entries_.end() && it->limit == limit) {
        touch(abs);
        return &it.value();
    }
    // Предел сменили: копия в памяти ужата не так, как надо.
    if (it != entries_.end()) drop(abs);

    // Только заголовок, и читается он ОДИН РАЗ — метаданными: размеры есть,
    // пикселей нет и не надо. Место под фотографию считается по НАСТОЯЩИМ
    // размерам, а не по размеру копии.
    ImageMetadata facts = ImageMetadata::fromFile(abs);
    const QSize declared = facts.size();
    if (declared.isEmpty()) {
        // Файла нет — рамка, и место под неё держится. Файл, который есть, но
        // картинкой не является, — не наше дело: строка остаётся строкой.
        if (facts.exists()) return nullptr;
        Record gone;
        gone.facts = std::move(facts);
        gone.limit = limit;
        gone.state = State::Missing;
        entries_.insert(abs, std::move(gone));
        order_.prepend(abs);
        const auto lost = entries_.constFind(abs);
        return lost == entries_.constEnd() ? nullptr : &lost.value();
    }

    Record entry;
    entry.declared = declared;
    entry.facts = std::move(facts);
    entry.limit = limit;
    entry.state = State::Pending;
    entries_.insert(abs, std::move(entry));
    order_.prepend(abs);
    const auto found = entries_.constFind(abs);
    return found == entries_.constEnd() ? nullptr : &found.value();
}

const ZImageCache::Record* ZImageCache::peek(const QString& abs) const {
    const auto it = entries_.constFind(abs);
    return it == entries_.constEnd() ? nullptr : &it.value();
}

void ZImageCache::plan(const void* owner) {
    // Пропавшие файлы перепроверяем: рамка «файл не найден» не имеет права
    // застыть навсегда. Стоит проверка одного обращения к файловой системе на
    // картинку, и то не на каждый кадр, а на пересчёт места.
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (it->state == State::Missing && QFileInfo::exists(it.key())) {
            order_.removeAll(it.key());
            it = entries_.erase(it);
        } else {
            ++it;
        }
    }

    const auto mineIt = protectedBy_.constFind(owner);
    if (mineIt == protectedBy_.constEnd()) return;
    const int limit = sideLimit();

    // И обратное: файл картинки открытой заметки унесли, пока она лежала в
    // кэше (кэш один на программу, и другой вид его уже видел). Разжатую копию
    // отпускаем, запись становится «файла нет» — рамка, место держится.
    for (const QString& key : mineIt.value()) {
        auto it = entries_.find(key);
        if (it == entries_.end() || it->state == State::Missing) continue;
        if (QFileInfo::exists(key)) continue;
        bytes_ -= it->bytes;
        it->bytes = 0;
        it->image = QImage();
        it->state = State::Missing;
        it->facts = ImageMetadata::fromFile(key);
    }

    // Все картинки этой заметки — от самой лёгкой к самой тяжёлой: так их
    // покажется больше всего.
    std::vector<std::pair<qint64, QString>> mine;
    for (const QString& key : mineIt.value()) {
        const auto it = entries_.constFind(key);
        if (it == entries_.constEnd()) continue;
        mine.push_back({decodedBytes(it->declared, limit), key});
    }
    std::sort(mine.begin(), mine.end());

    qint64 taken = 0;
    for (const auto& [cost, key] : mine) {
        auto it = entries_.find(key);
        if (it == entries_.end()) continue;
        // Отказ Qt разжимать и уже принятое решение «рамка» не пересматриваем:
        // иначе одна и та же картинка мигала бы туда-сюда при каждой правке.
        if (it->state == State::TooBig || it->state == State::Crowded) continue;
        // Потолок Qt на разжатие — тоже по заголовку, до всякого чтения
        // пикселей, по ПОЛНОМУ размеру: решить это надо здесь, место под
        // картинку резервируется до первой отрисовки.
        const qint64 full = qint64(it->declared.width()) * it->declared.height() * 4;
        if (full > qint64(QImageReader::allocationLimit()) * 1024 * 1024) {
            it->state = State::TooBig;
            continue;
        }
        taken += cost;
        if (taken > budgetBytes_) it->state = State::Crowded;
    }
}

// Пиксели — лениво, по первому рисованию. Разжимать всю заметку при открытии
// незачем: замер на 25 снимках дал 2.7 с, а видно из них один-два.
const QImage* ZImageCache::pixels(const QString& key) {
    auto it = entries_.find(key);
    if (it == entries_.end()) return nullptr;
    if (it->state == State::Shown) return &it->image;
    if (it->state != State::Pending) return nullptr;

    const QSize declared = it->declared;
    const int limit = sideLimit();
    const qint64 cost = decodedBytes(declared, limit);
    // Место под неё — за счёт чужих заметок: защищённые не трогаем никогда.
    trim(key, cost);
    if (bytes_ + cost > budgetBytes_) {
        // Не влезла даже после вытеснения — рамка. Спрашиваем ДО разжатия:
        // иначе платили бы памятью ровно за то, чего решили не показывать.
        it = entries_.find(key);
        if (it != entries_.end()) it->state = State::Crowded;
        return nullptr;
    }

    QElapsedTimer decode;
    decode.start();
    DecodeRequest request;
    if (limit > 0 && (declared.width() > limit || declared.height() > limit)) {
        // Предел держит ОБЕ стороны, только вниз. Просим об этом сам читатель —
        // иные форматы умеют разжимать сразу в нужный размер.
        QSize scaled = declared.scaled(limit, limit, Qt::KeepAspectRatio);
        scaled.setWidth(qMax(1, scaled.width()));
        scaled.setHeight(qMax(1, scaled.height()));
        request.maxSize = scaled;
    }
    // Глубина показу не нужна: экран восьмибитный, а шестнадцать бит стоят
    // четверти времени и вдвое больше памяти в кэше. Ввоз просит их отдельно.
    QImage image = decodeImageFile(key, request);
    // Читатель отдал не меньше просимого (у JPEG размер идёт восьмыми долями) —
    // доводим до предела сами.
    if (!request.maxSize.isEmpty() && image.size() != request.maxSize &&
        (image.width() > request.maxSize.width() || image.height() > request.maxSize.height())) {
        image = image.scaled(request.maxSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    ++decodes_;
    decodeMicros_ += decode.nsecsElapsed() / 1000;

    it = entries_.find(key);
    if (it == entries_.end()) return nullptr;
    if (image.isNull()) {
        // Отказ Qt разжимать: картинка больше потолка. Запоминаем, чтобы не
        // спрашивать заново на каждом кадре.
        it->state = State::TooBig;
        return nullptr;
    }
    it->bytes = qint64(image.sizeInBytes());
    it->image = std::move(image);
    it->state = State::Shown;
    // Раз уж картинку всё равно разжали — забираем и то, что видно только у
    // разжатой копии: цветовое пространство и глубину.
    it->facts.addDecoded(it->image);
    bytes_ += it->bytes;
    touch(key);
    return &it->image;
}

void ZImageCache::resetCounters() {
    decodes_ = 0;
    decodeMicros_ = 0;
}

void ZImageCache::clear() {
    entries_.clear();
    order_.clear();
    bytes_ = 0;
}

}  // namespace zametti
