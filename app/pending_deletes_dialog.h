// Вопрос предохранителя массового удаления — один из двух вопросов всего
// синка (решение владельца). Полноценное окно, а не QMessageBox: список
// ПОЛНЫХ имён (root/папка/…/заметка) с прокруткой, окно тянется, как About, —
// человек должен оценить ущерб глазами целиком, а не по «… и ещё 13».
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

protected:
    explicit PendingDeletesDialog(QWidget* parent, const QStringList& qualifiedNames);

    Verdict verdict_ = Verdict::DecideLater;
};

}  // namespace zametti

#endif  // ZAMETTI_PENDING_DELETES_DIALOG_H
