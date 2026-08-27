#include "pending_deletes_dialog.h"

#include "dialog_font.h"

#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

namespace zametti {

// Сравнение без регистра и краёв: человек печатает слово, а не проходит
// тест на аккуратность пробелов.
PendingDeletesDialog::Verdict PendingDeletesDialog::verdictFor(const QString& typed) {
    const QString word = typed.trimmed().toLower();
    if (word == QStringLiteral("remove")) return Verdict::DeleteHere;
    if (word == QStringLiteral("restore")) return Verdict::KeepAlive;
    return Verdict::DecideLater;
}

PendingDeletesDialog::PendingDeletesDialog(QWidget* parent, const QStringList& qualifiedNames)
    : QDialog(parent) {
    setWindowTitle(QStringLiteral("Sync wants to delete notes"));
    // Кегль — из настроек: системный дефолт на FullHD мельче остального окна.
    setFont(dialogFont());

    auto* heading = new QLabel(
        QStringLiteral("Another device deleted %1 notes:").arg(qualifiedNames.size()), this);
    heading->setWordWrap(true);

    // Список только для глаз: ни выбора, ни правки — решение одно на всех.
    auto* list = new QListWidget(this);
    list->setSelectionMode(QAbstractItemView::NoSelection);
    list->setFocusPolicy(Qt::NoFocus);
    list->setUniformItemSizes(true);
    list->addItems(qualifiedNames);

    auto* instruction = new QLabel(
        QStringLiteral("To remove the local copies, type `remove`. "
                       "To restore the files in the cloud, type `restore`."),
        this);
    instruction->setWordWrap(true);

    auto* word = new QLineEdit(this);
    word->setPlaceholderText(QStringLiteral("remove / restore"));

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok, this);
    QPushButton* ok = buttons->button(QDialogButtonBox::Ok);
    ok->setEnabled(false);

    connect(word, &QLineEdit::textChanged, this, [ok, word] {
        ok->setEnabled(verdictFor(word->text()) != Verdict::DecideLater);
    });
    const auto decide = [this, word] {
        const Verdict typed = verdictFor(word->text());
        if (typed == Verdict::DecideLater) return;  // Enter в пустом поле — не решение
        verdict_ = typed;
        accept();
    };
    connect(ok, &QPushButton::clicked, this, decide);
    connect(word, &QLineEdit::returnPressed, this, decide);
    // Esc и крестик — «решу позже»: verdict_ остаётся DecideLater.

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(heading);
    layout->addWidget(list, 1);
    layout->addWidget(instruction);
    layout->addWidget(word);
    layout->addWidget(buttons);
    resize(560, 520);
    word->setFocus();
}

PendingDeletesDialog::Verdict PendingDeletesDialog::ask(QWidget* parent,
                                                        const QStringList& qualifiedNames) {
    PendingDeletesDialog dialog(parent, qualifiedNames);
    dialog.exec();
    return dialog.verdict_;
}

}  // namespace zametti
