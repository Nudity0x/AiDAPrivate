#pragma once

#include <QDialog>
#include <QToolButton>

#include <QString>

class QLineEdit;
class QListWidget;
class QVBoxLayout;

namespace aida::qt::ai {

class AidaPillPopup : public QDialog {
    Q_OBJECT
public:
    explicit AidaPillPopup(QWidget* anchor, QWidget* content);

    void openUnderAnchor();

private:
    QWidget* anchor_ = nullptr;
};

class AidaModelPill : public QToolButton {
    Q_OBJECT
public:
    explicit AidaModelPill(QWidget* parent = nullptr);

    void refresh();
    QString providerId() const;
    bool hasSelection() const;
    bool authed() const;

Q_SIGNALS:
    void openSettingsForProvider(const QString& provider_id);

protected:
    bool event(QEvent* event) override;

private:
    void openPopup();
    QString modelId() const;
    QString pillLabel() const;
    QString tooltipText() const;
};

class AidaAgentPill : public QToolButton {
    Q_OBJECT
public:
    explicit AidaAgentPill(QWidget* parent = nullptr);

    void refresh();

protected:
    bool event(QEvent* event) override;

private:
    QString pillLabel() const;
    QString tooltipText() const;
};

class AidaSkillsPill : public QToolButton {
    Q_OBJECT
public:
    explicit AidaSkillsPill(QWidget* parent = nullptr);

protected:
    bool event(QEvent* event) override;

private:
    void openPopup();
};

class AidaMcpPill : public QToolButton {
    Q_OBJECT
public:
    explicit AidaMcpPill(QWidget* parent = nullptr);

    void refresh();

protected:
    bool event(QEvent* event) override;

private:
    void openPopup();
    QString pillLabel() const;
    QString tooltipText() const;
};

}
