#pragma once

#include "aida_line_edit.hpp"

class QAction;
class QLabel;

namespace aida::qt::widgets {

class AidaSearchField : public AidaLineEdit
{
    Q_OBJECT
public:
    explicit AidaSearchField(QWidget* parent = nullptr);
    explicit AidaSearchField(const QString& placeholder, QWidget* parent = nullptr);

    QLabel* matchCountLabel() const;
    void setMatchCount(qint64 total, qint64 active = -1);
    void clearMatchCount();

Q_SIGNALS:
    void cleared();

protected:
    void focusInEvent(QFocusEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    void changeEvent(QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    void refreshLeadingIcon(bool focused);
    void updateClearVisibility();

    QAction* leading_action_ = nullptr;
    QAction* clear_action_ = nullptr;
    QLabel* match_label_ = nullptr;
    qint64 match_total_ = -1;
    qint64 match_active_ = -1;
};

}
