#include "welcomepage.h"

#include <QCheckBox>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPixmap>
#include <QPushButton>
#include <QVBoxLayout>

WelcomePage::WelcomePage(QWidget *parent) : QWidget(parent) {
    auto *outer = new QVBoxLayout(this);
    outer->addStretch(1);
    auto *center = new QHBoxLayout();
    center->addStretch(1);
    auto *col = new QVBoxLayout();
    col->setSpacing(10);
    center->addLayout(col, 2); // narrow column, stretches take the rest
    center->addStretch(1);
    outer->addLayout(center);
    outer->addStretch(2);

    auto *titleRow = new QHBoxLayout();
    auto *icon = new QLabel(this);
    QPixmap pm(QStringLiteral(":/icons/app.png"));
    if (!pm.isNull())
        icon->setPixmap(pm.scaled(64, 64, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    auto *title = new QLabel(QStringLiteral("booru-explorer"), this);
    QFont tf = title->font();
    tf.setPointSize(tf.pointSize() + 12);
    tf.setBold(true);
    title->setFont(tf);
    titleRow->addStretch(1);
    titleRow->addWidget(icon);
    titleRow->addWidget(title);
    titleRow->addStretch(1);
    col->addLayout(titleRow);

    m_resume = new QPushButton(this);
    m_resume->setCursor(Qt::PointingHandCursor);
    connect(m_resume, &QPushButton::clicked, this,
            [this] { emit openPathRequested(m_lastRoot); });
    col->addWidget(m_resume);

    auto *recLabel = new QLabel(QStringLiteral("Recent archives"), this);
    recLabel->setStyleSheet(QStringLiteral("color:#4a5068;font-size:11px;"));
    col->addWidget(recLabel);
    m_recent = new QListWidget(this);
    m_recent->setMaximumHeight(220);
    connect(m_recent, &QListWidget::itemActivated, this, [this](QListWidgetItem *it) {
        emit openPathRequested(it->data(Qt::UserRole).toString());
    });
    connect(m_recent, &QListWidget::itemClicked, this, [this](QListWidgetItem *it) {
        emit openPathRequested(it->data(Qt::UserRole).toString());
    });
    col->addWidget(m_recent);

    auto *openBtn = new QPushButton(QStringLiteral("Open folder…"), this);
    openBtn->setDefault(true);
    connect(openBtn, &QPushButton::clicked, this, &WelcomePage::openDialogRequested);
    col->addWidget(openBtn);

    m_always = new QCheckBox(QStringLiteral("Always load last session on startup"), this);
    connect(m_always, &QCheckBox::toggled, this, &WelcomePage::restoreToggled);
    col->addWidget(m_always);
}

void WelcomePage::setHistory(const QStringList &history, const QString &lastRoot) {
    m_lastRoot = lastRoot;
    const bool canResume = !lastRoot.isEmpty() && QDir(lastRoot).exists();
    m_resume->setVisible(canResume);
    if (canResume)
        m_resume->setText(QStringLiteral("Resume last session\n%1").arg(lastRoot));

    m_recent->clear();
    int shown = 0;
    for (const QString &p : history) {
        if (p == lastRoot || !QDir(p).exists())
            continue; // resume button covers lastRoot; skip dead entries
        if (shown >= 10)
            break;
        auto *it = new QListWidgetItem(
            QStringLiteral("%1\n%2").arg(QFileInfo(p).fileName(), p));
        it->setData(Qt::UserRole, p);
        it->setToolTip(p);
        m_recent->addItem(it);
        shown++;
    }
    m_recent->setVisible(shown > 0);
}

void WelcomePage::setRestore(bool on) {
    m_always->blockSignals(true);
    m_always->setChecked(on);
    m_always->blockSignals(false);
}
