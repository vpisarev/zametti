// Вопрос предохранителя массового удаления — один из двух вопросов всего
// синка (решение владельца). Полноценное окно, а не QMessageBox: список
// ПОЛНЫХ имён (root/папка/…/заметка) с прокруткой, окно тянется, как About, —
// человек должен оценить ущерб глазами целиком, а не по «… и ещё 13».
//
// Решение ПЕЧАТАЕТСЯ СЛОВОМ, а не кнопкой (решение владельца): не ту кнопку
// нажимают на автомате, а слово remove/restore на автомате не набирают.
// Ok оживает, только когда в поле ровно одно из двух слов.
//
// Три исхода, и третий — не мелочь: закрыть окно значит «решу позже» —
// ничего не удаляется и не воскрешается, пометки живы, следующий прогон
// спросит снова.

#ifndef ZAMETTI_PENDING_DELETES_DIALOG_H
#define ZAMETTI_PENDING_DELETES_DIALOG_H

#include <QDialog>
#include <QStringList>

namespace zametti {

class PendingDeletesDialog : public QDialog {
    Q_OBJECT

public:
    enum class Verdict {
        DeleteHere,   // подтвердил: применить удаления и здесь
        KeepAlive,    // отказал: объявить заметки живыми, облако вылечится
        DecideLater,  // закрыл окно: не решил, спросим в следующий раз
    };

    static Verdict ask(QWidget* parent, const QStringList& qualifiedNames);

    // Словарь решения — одной проверяемой функцией: remove → удалить,
    // restore → оставить, всё прочее (регистр и края прощаются) — не решение.
    static Verdict verdictFor(const QString& typed);

    // Открытый конструктор — наборам: ask() модален, а проверке нужно
    // подержать окно в руках (поле, кнопка, вердикт).
    explicit PendingDeletesDialog(QWidget* parent, const QStringList& qualifiedNames);
    Verdict verdict() const { return verdict_; }

protected:
    Verdict verdict_ = Verdict::DecideLater;
};

}  // namespace zametti

#endif  // ZAMETTI_PENDING_DELETES_DIALOG_H
