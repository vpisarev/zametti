#include "sort_order.h"

namespace zametti {
namespace {

// Таблица, а не три ветки if. Она же задаёт и написание в файле, и порядок
// разбора: добавить ключ — дописать строку, и оба конца круга сойдутся сами.
struct KeyName {
    SortKey key;
    QLatin1StringView text;
};

constexpr KeyName kKeys[] = {
    {SortKey::Name, QLatin1StringView("name")},
    {SortKey::Modified, QLatin1StringView("modified")},
    {SortKey::Created, QLatin1StringView("created")},
};

}  // namespace

bool defaultAscending(SortKey key) { return key == SortKey::Name; }

SortOrder defaultOrder(SortKey key) { return SortOrder{key, defaultAscending(key)}; }

QString sortOrderToString(SortOrder order) {
    for (const KeyName& item : kKeys) {
        if (item.key != order.key) continue;
        return QString(item.text) +
               (order.ascending ? QStringLiteral("-asc") : QStringLiteral("-desc"));
    }
    return QString();
}

std::optional<SortOrder> parseSortOrder(QStringView text) {
    const QStringView trimmed = text.trimmed();
    if (trimmed.isEmpty()) return std::nullopt;

    // Направление отрезаем с хвоста, а не ищем дефис: дефис есть и в самом
    // ключе будущих версий, и разбор по первому попавшемуся ломался бы молча.
    SortOrder order;
    QStringView head;
    if (trimmed.endsWith(QLatin1StringView("-asc"), Qt::CaseInsensitive)) {
        order.ascending = true;
        head = trimmed.first(trimmed.size() - 4);
    } else if (trimmed.endsWith(QLatin1StringView("-desc"), Qt::CaseInsensitive)) {
        order.ascending = false;
        head = trimmed.first(trimmed.size() - 5);
    } else {
        // Ключ без направления — законная краткая запись для руки: «sort: name».
        // Направление берётся по умолчанию для этого ключа.
        head = trimmed;
        for (const KeyName& item : kKeys) {
            if (head.compare(item.text, Qt::CaseInsensitive) != 0) continue;
            return defaultOrder(item.key);
        }
        return std::nullopt;
    }

    for (const KeyName& item : kKeys) {
        if (head.compare(item.text, Qt::CaseInsensitive) != 0) continue;
        order.key = item.key;
        return order;
    }
    return std::nullopt;
}

SortOrder pressedSort(SortOrder now, SortKey pressed) {
    if (now.key != pressed) return defaultOrder(pressed);
    return SortOrder{pressed, !now.ascending};
}

void applySortMark(NoteHeader& header, std::optional<SortOrder> order) {
    header.setPresent(true);
    if (!order.has_value()) {
        header.unset("sort");
        return;
    }
    header.set("sort", sortOrderToString(*order).toStdString());
}

QString sortOrderTitle(SortOrder order) {
    switch (order.key) {
        case SortKey::Name:
            return order.ascending ? QStringLiteral("By name, A→Z")
                                   : QStringLiteral("By name, Z→A");
        case SortKey::Modified:
            return order.ascending ? QStringLiteral("By modified date, oldest first")
                                   : QStringLiteral("By modified date, newest first");
        case SortKey::Created:
            return order.ascending ? QStringLiteral("By created date, oldest first")
                                   : QStringLiteral("By created date, newest first");
    }
    return QString();
}

}  // namespace zametti
