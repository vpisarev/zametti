#include "pending_deletes_dialog.h"

#include <QDialogButtonBox>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

namespace zametti {

PendingDeletesDialog::PendingDeletesDialog(QWidget* parent, const QStringList& qualifiedNames)
    : QDialog(parent) {
    setWindowTitle(QStringLiteral("Sync wants to delete notes"));

    auto* heading = new QLabel(
        QStringLiteral("Another device deleted %1 notes. Delete them here too?")
            .arg(qualifiedNames.size()),
        this);
    heading->setWordWrap(true);

    // Список только для глаз: ни выбора, ни правки — решение одно на всех.
    auto* list = new QListWidget(this);
    list->setSelectionMode(QAbstractItemView::NoSelection);
    list->setFocusPolicy(Qt::NoFocus);
    list->setUniformItemSizes(true);
    list->addItems(qualifiedNames);

    auto* buttons = new QDialogButtonBox(this);
    QPushButton* keep =
        buttons->addButton(QStringLiteral("Keep the notes"), QDialogButtonBox::RejectRole);
    QPushButton* remove =
        buttons->addButton(QStringLiteral("Delete here too"), QDialogButtonBox::DestructiveRole);
    // Дефолт — безопасный: Enter оставляет заметки. Удаление — только
    // осознанным нажатием именно этой кнопки.
    keep->setDefault(true);
    connect(keep, &QPushButton::clicked, this, [this] {
        verdict_ = Verdict::KeepAlive;
        accept();
    });
    connect(remove, &QPushButton::clicked, this, [this] {
        verdict_ = Verdict::DeleteHere;
        accept();
    });
    // Esc и крестик — «решу позже»: verdict_ остаётся DecideLater.

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(heading);
    layout->addWidget(list, 1);
    layout->addWidget(buttons);
    resize(560, 480);
}

PendingDeletesDialog::Verdict PendingDeletesDialog::ask(QWidget* parent,
                                                        const QStringList& qualifiedNames) {
    PendingDeletesDialog dialog(parent, qualifiedNames);
    dialog.exec();
    return dialog.verdict_;
}

}  // namespace zametti
