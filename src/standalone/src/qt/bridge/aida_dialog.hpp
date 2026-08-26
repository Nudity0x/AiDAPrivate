#pragma once

#include <QDialog>
#include <QPointer>
#include <QString>

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

class QTimer;

namespace aida::qt::bridge {

class AidaDialog : public QDialog {
    Q_OBJECT
public:
    class RevalidateScope {
    public:
        struct hooks_t {
            std::function<QString()> identity_fn;
            std::function<quint64()> generation_fn;
        };

        RevalidateScope(AidaDialog& dialog, hooks_t hooks, QString stale_message);
        ~RevalidateScope();

        RevalidateScope(const RevalidateScope&) = delete;
        RevalidateScope& operator=(const RevalidateScope&) = delete;

        bool valid() const;
        bool check_now();

    private:
        bool stale() const;

        AidaDialog* dialog_ = nullptr;
        hooks_t hooks_;
        QString stale_message_;
        QString captured_identity_;
        quint64 captured_generation_ = 0;
        bool has_identity_ = false;
        bool has_generation_ = false;
        QTimer* timer_ = nullptr;
        bool detached_ = false;
    };

    explicit AidaDialog(QWidget* parent = nullptr,
                        Qt::WindowFlags flags = Qt::WindowFlags());
    ~AidaDialog() override;

    RevalidateScope& add_revalidate_scope(RevalidateScope::hooks_t hooks,
                                          QString stale_message);
    void accept() override;

    using notification_hook_t = std::function<void(const QString& message)>;
    static void set_notification_hook(notification_hook_t hook);
    void notify_error(const QString& message);

Q_SIGNALS:
    void validationStale(const QString& message);

private:
    bool revalidate_all();

    std::vector<std::unique_ptr<RevalidateScope>> scopes_;
    static notification_hook_t notification_hook_;
};

}
