#pragma once

// A menu whose checkable entries toggle in place instead of dismissing the
// popup. Picking sources ("Boorus…") is a set of toggles, so closing after
// every click would force the user to reopen the menu for each booru.
// Non-checkable entries, separators, Enter and clicks outside close as usual.
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>

class MultiPickMenu : public QMenu {
public:
    explicit MultiPickMenu(QWidget *parent = nullptr) : QMenu(parent) {}

protected:
    void mouseReleaseEvent(QMouseEvent *e) override {
        QAction *act = actionAt(e->pos());
        if (act && act->isEnabled() && act->isCheckable()) {
            // trigger() flips the check state and re-emits triggered() on
            // this menu, which is what the callers listen to.
            act->trigger();
            e->accept();
            return;
        }
        QMenu::mouseReleaseEvent(e);
    }
    void keyPressEvent(QKeyEvent *e) override {
        // Space toggles without closing; Enter still accepts and closes.
        if (e->key() == Qt::Key_Space) {
            QAction *act = activeAction();
            if (act && act->isEnabled() && act->isCheckable()) {
                act->trigger();
                e->accept();
                return;
            }
        }
        QMenu::keyPressEvent(e);
    }
};
