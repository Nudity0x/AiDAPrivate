#include "qt/net/qt_scanner_new_audit_dialog.hpp"

#include <QAbstractSpinBox>
#include <QApplication>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTableView>
#include <QVBoxLayout>

#include <algorithm>
#include <cctype>
#include <utility>

#include "qt/net/qt_human_request_editor.hpp"
#include "qt/net/qt_scanner_view.hpp"
#include "qt/network/shared/style_helpers.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_button.hpp"

namespace aida::qt::net {

namespace {

bool validAuditUrl(std::string_view url)
{
    const std::size_t schemeEnd = url.find("://");
    if (schemeEnd == std::string_view::npos)
        return false;
    std::string scheme(url.substr(0, schemeEnd));
    std::transform(scheme.begin(), scheme.end(), scheme.begin(), [](unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    if (scheme != "http" && scheme != "https")
        return false;
    const std::size_t hostBegin = schemeEnd + 3;
    if (hostBegin >= url.size())
        return false;
    const std::size_t hostEnd = url.find_first_of("/:?#", hostBegin);
    return (hostEnd == std::string_view::npos ? url.size() : hostEnd) > hostBegin;
}

QLabel* fieldError(QWidget* parent, const QString& objectName)
{
    auto* label = new QLabel(parent);
    label->setObjectName(objectName);
    label->setProperty("aidaVariant", QStringLiteral("error"));
    label->setWordWrap(true);
    label->setVisible(false);
    return label;
}

}

QtScannerModuleModel::QtScannerModuleModel(QObject* parent)
    : QAbstractTableModel(parent) {}

void QtScannerModuleModel::adopt(std::vector<aida::burp::scanner::module_t> modules)
{
    beginResetModel();
    modules_ = std::move(modules);
    endResetModel();
}

std::size_t QtScannerModuleModel::enabledCount() const noexcept
{
    return modules_.size() - disabled_.size();
}

std::vector<std::string> QtScannerModuleModel::enabledIds() const
{
    std::vector<std::string> ids;
    ids.reserve(modules_.size());
    for (const auto& module : modules_) {
        if (disabled_.find(module.id) == disabled_.end())
            ids.push_back(module.id);
    }
    return ids;
}

int QtScannerModuleModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(modules_.size());
}

int QtScannerModuleModel::columnCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : 1;
}

QVariant QtScannerModuleModel::data(const QModelIndex& index, int role) const
{
    if (index.row() < 0 || index.row() >= static_cast<int>(modules_.size()))
        return {};
    const auto& module = modules_[static_cast<std::size_t>(index.row())];
    if (role == Qt::DisplayRole || role == Qt::ToolTipRole) {
        return QStringLiteral("%1  [%2] %3")
            .arg(QString::fromStdString(module.name))
            .arg(QString::fromStdString(module.category))
            .arg(QString::fromStdString(module.id));
    }
    if (role == Qt::CheckStateRole)
        return disabled_.find(module.id) == disabled_.end() ? Qt::Checked : Qt::Unchecked;
    if (role == Qt::ForegroundRole)
        return theme::tokens().text_secondary;
    return {};
}

bool QtScannerModuleModel::setData(const QModelIndex& index, const QVariant& value,
                                   int role)
{
    if (role != Qt::CheckStateRole || index.row() < 0 ||
        index.row() >= static_cast<int>(modules_.size()))
        return false;
    const auto& id = modules_[static_cast<std::size_t>(index.row())].id;
    if (value.toInt() == Qt::Checked)
        disabled_.erase(id);
    else
        disabled_.insert(id);
    Q_EMIT dataChanged(index, index, { Qt::CheckStateRole });
    return true;
}

Qt::ItemFlags QtScannerModuleModel::flags(const QModelIndex& index) const
{
    if (!index.isValid())
        return Qt::NoItemFlags;
    return Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable;
}

QtNewAuditDialog::QtNewAuditDialog(QtScannerController* controller, QWidget* parent)
    : AidaDialog(parent), controller_(controller)
{
    setWindowTitle(QStringLiteral("New Scanner Audit"));
    setObjectName(QStringLiteral("dialog.network_scanner_new"));
    setMinimumWidth(dialog_min_width_chars(this, 56));
    setMinimumHeight(editor_min_height_lines(this, 22));
    resize(dialog_min_width_chars(this, 88), editor_min_height_lines(this, 40));

    const auto& t = theme::tokens();
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(t.panel.padding, t.panel.padding, t.panel.padding,
        t.panel.padding);
    layout->setSpacing(t.spacing.xs);

    auto* urlLabel = new QLabel(QStringLiteral("Target URL"), this);
    urlLabel->setProperty("aidaVariant", QStringLiteral("secondary"));
    layout->addWidget(urlLabel);
    urlEdit_ = new QLineEdit(this);
    urlEdit_->setObjectName(QStringLiteral("scanner-new-url"));
    urlEdit_->setMaxLength(1023);
    urlEdit_->setPlaceholderText(QStringLiteral("https://example.com/path?id=1"));
    layout->addWidget(urlEdit_);
    urlError_ = fieldError(this, QStringLiteral("scanner-new-url.error"));
    layout->addWidget(urlError_);

    auto* requestLabel = new QLabel(QStringLiteral("Raw Request (HTTP/1.1 textual)"), this);
    requestLabel->setProperty("aidaVariant", QStringLiteral("secondary"));
    layout->addWidget(requestLabel);
    requestEditor_ = new QtHumanRequestEditor(this);
    QtHumanRequestEditor::Config editorConfig;
    editorConfig.stableId = QStringLiteral("scanner-new-request");
    editorConfig.maxBytes = 65535;
    editorConfig.editable = true;
    requestEditor_->setConfig(editorConfig);
    requestEditor_->setMinimumHeight(editor_min_height_lines(requestEditor_, 8));
    layout->addWidget(requestEditor_, 1);
    requestError_ = fieldError(this, QStringLiteral("scanner-new-request.error"));
    layout->addWidget(requestError_);

    auto* togglesRow = new QHBoxLayout();
    togglesRow->setSpacing(t.spacing.lg);
    scopeOnlyCheck_ = new QCheckBox(QStringLiteral("Scope only"), this);
    scopeOnlyCheck_->setObjectName(QStringLiteral("scanner-new-scope"));
    scopeOnlyCheck_->setChecked(true);
    togglesRow->addWidget(scopeOnlyCheck_);
    followRedirectsCheck_ = new QCheckBox(QStringLiteral("Follow redirects"), this);
    followRedirectsCheck_->setObjectName(QStringLiteral("scanner-new-redirects"));
    togglesRow->addWidget(followRedirectsCheck_);
    togglesRow->addStretch(1);
    layout->addLayout(togglesRow);

    auto* limitsGrid = new QHBoxLayout();
    limitsGrid->setSpacing(t.spacing.sm);
    const auto limitField = [&](const QString& label, const QString& objectName,
                                int minimum, int maximum, int value,
                                QLabel*& errorOut) -> QSpinBox* {
        auto* column = new QVBoxLayout();
        column->setSpacing(t.spacing.xxs);
        auto* caption = new QLabel(label, this);
        caption->setProperty("aidaVariant", QStringLiteral("secondary"));
        column->addWidget(caption);
        auto* spin = new QSpinBox(this);
        spin->setObjectName(objectName);
        spin->setRange(minimum, maximum);
        spin->setValue(value);
        column->addWidget(spin);
        errorOut = fieldError(this, objectName + QStringLiteral(".error"));
        column->addWidget(errorOut);
        limitsGrid->addLayout(column, 1);
        return spin;
    };
    timeoutSpin_ = limitField(QStringLiteral("Timeout (ms)"),
        QStringLiteral("scanner-new-timeout"), 100, 300000, 15000, timeoutError_);
    concurrencySpin_ = limitField(QStringLiteral("Max parallel"),
        QStringLiteral("scanner-new-concurrency"), 1, 64, 16, concurrencyError_);
    throttleSpin_ = limitField(QStringLiteral("Throttle (ms)"),
        QStringLiteral("scanner-new-throttle"), 0, 60000, 0, throttleError_);
    moduleCapSpin_ = limitField(QStringLiteral("Per-module cap"),
        QStringLiteral("scanner-new-module-cap"), 1, 100000, 64, moduleCapError_);
    layout->addLayout(limitsGrid);

    auto* modulesLabel = new QLabel(QStringLiteral("Enabled modules"), this);
    modulesLabel->setProperty("aidaVariant", QStringLiteral("secondary"));
    layout->addWidget(modulesLabel);
    moduleModel_ = new QtScannerModuleModel(this);
    modulesView_ = new QTableView(this);
    modulesView_->setObjectName(QStringLiteral("scanner-new-modules"));
    modulesView_->horizontalHeader()->hide();
    modulesView_->horizontalHeader()->setStretchLastSection(true);
    modulesView_->verticalHeader()->hide();
    modulesView_->verticalHeader()->setDefaultSectionSize(t.table.compact_row_h);
    modulesView_->setAlternatingRowColors(true);
    modulesView_->setModel(moduleModel_);
    modulesView_->setMinimumHeight(t.table.compact_row_h * 5);
    modulesView_->setMaximumHeight(t.table.compact_row_h * 6 + t.spacing.lg);
    layout->addWidget(modulesView_);
    modulesError_ = fieldError(this, QStringLiteral("scanner-new-modules.error"));
    layout->addWidget(modulesError_);

    summaryLabel_ = new QLabel(this);
    summaryLabel_->setObjectName(QStringLiteral("scanner-new-summary"));
    summaryLabel_->setProperty("aidaVariant", QStringLiteral("error"));
    summaryLabel_->setWordWrap(true);
    summaryLabel_->setVisible(false);
    layout->addWidget(summaryLabel_);
    noticeLabel_ = new QLabel(this);
    noticeLabel_->setProperty("aidaVariant", QStringLiteral("warning"));
    noticeLabel_->setWordWrap(true);
    noticeLabel_->setVisible(false);
    layout->addWidget(noticeLabel_);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
        this);
    startButton_ = buttons->button(QDialogButtonBox::Ok);
    startButton_->setText(QStringLiteral("Start Audit"));
    cancelButton_ = buttons->button(QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &QtNewAuditDialog::onAccept);
    connect(buttons, &QDialogButtonBox::rejected, this, [this] {
        if (!pending_)
            reject();
    });
    layout->addWidget(buttons);

    const auto revalidate = [this](auto&&...) { validateForm(); updateAcceptState(); };
    connect(urlEdit_, &QLineEdit::textChanged, this, revalidate);
    connect(requestEditor_, &QtHumanRequestEditor::validityChanged, this,
        [this](bool, const QString&) { validateForm(); updateAcceptState(); });
    connect(requestEditor_, &QtHumanRequestEditor::hasUnappliedPrettyChanged, this,
        [this](bool) { updateAcceptState(); });
    connect(requestEditor_, &QtHumanRequestEditor::authorityChanged, this, revalidate);
    connect(moduleModel_, &QAbstractTableModel::dataChanged, this, revalidate);
    connect(timeoutSpin_, &QSpinBox::valueChanged, this, revalidate);
    connect(concurrencySpin_, &QSpinBox::valueChanged, this, revalidate);
    connect(throttleSpin_, &QSpinBox::valueChanged, this, revalidate);
    connect(moduleCapSpin_, &QSpinBox::valueChanged, this, revalidate);
}

void QtNewAuditDialog::openStaged(const QString& url, const QString& rawRequest,
                                  std::uint64_t generation)
{
    generation_ = generation;
    if (moduleModel_->rowCount() == 0)
        moduleModel_->adopt(aida::burp::scanner::all_modules());
    if (!url.isEmpty())
        urlEdit_->setText(url);
    if (!rawRequest.isEmpty()) {
        requestEditor_->setAuthority(
            QStringLiteral("scanner.new-request.%1").arg(static_cast<quint64>(generation_)),
            rawRequest);
    }
    noticeLabel_->setVisible(false);
    pending_ = false;
    validateForm();
    updateAcceptState();
    open();
}

void QtNewAuditDialog::setPending(bool pending)
{
    pending_ = pending;
    urlEdit_->setEnabled(!pending);
    requestEditor_->setEditable(!pending);
    scopeOnlyCheck_->setEnabled(!pending);
    followRedirectsCheck_->setEnabled(!pending);
    timeoutSpin_->setEnabled(!pending);
    concurrencySpin_->setEnabled(!pending);
    throttleSpin_->setEnabled(!pending);
    moduleCapSpin_->setEnabled(!pending);
    modulesView_->setEnabled(!pending);
    cancelButton_->setEnabled(!pending);
    if (pending) {
        noticeLabel_->setText(QStringLiteral(
            "Starting audit — the reviewed target and request are queued in Task Center."));
        noticeLabel_->setVisible(true);
    }
    updateAcceptState();
}

void QtNewAuditDialog::validateForm()
{
    form_.clear();
    const std::string url = urlEdit_->text().toStdString();
    const std::string raw = requestEditor_->authority().toStdString();
    if (url.empty())
        form_.reject("scanner-new-url", "Enter the exact HTTP or HTTPS target URL.");
    else if (!validAuditUrl(url))
        form_.reject("scanner-new-url", "Use an absolute HTTP or HTTPS URL with a host.");
    if (raw.empty()) {
        form_.reject("scanner-new-request", "Enter the HTTP/1.1 request to audit.");
    } else {
        const std::size_t lineEnd = raw.find_first_of("\r\n");
        const std::string_view requestLine(raw.data(), lineEnd);
        const std::size_t firstSpace = requestLine.find(' ');
        const std::size_t secondSpace = firstSpace == std::string_view::npos
            ? std::string_view::npos : requestLine.find(' ', firstSpace + 1);
        if (firstSpace == 0 || firstSpace == std::string_view::npos ||
            secondSpace == std::string_view::npos || secondSpace == firstSpace + 1 ||
            secondSpace + 1 >= requestLine.size())
            form_.reject("scanner-new-request",
                "The first line must contain method, request target, and HTTP version.");
        else if (raw.find("\r\n\r\n") == std::string_view::npos)
            form_.reject("scanner-new-request",
                "Terminate the HTTP header block with an empty CRLF line.");
    }
    if (timeoutSpin_->value() < 100 || timeoutSpin_->value() > 300000)
        form_.reject("scanner-new-timeout", "Use a timeout from 100 to 300000 ms.");
    if (concurrencySpin_->value() < 1 || concurrencySpin_->value() > 64)
        form_.reject("scanner-new-concurrency", "Use 1 to 64 parallel requests.");
    if (throttleSpin_->value() < 0 || throttleSpin_->value() > 60000)
        form_.reject("scanner-new-throttle", "Use a throttle from 0 to 60000 ms.");
    if (moduleCapSpin_->value() < 1 || moduleCapSpin_->value() > 100000)
        form_.reject("scanner-new-module-cap", "Use a per-module cap from 1 to 100000.");
    if (moduleModel_->enabledCount() == 0)
        form_.reject("scanner-new-modules", "Enable at least one Scanner module.");

    const auto applyError = [this](QLabel* label, const char* fieldId) {
        const char* error = form_.error_for(fieldId);
        label->setText(error ? QString::fromUtf8(error) : QString());
        label->setVisible(error != nullptr);
    };
    applyError(urlError_, "scanner-new-url");
    applyError(requestError_, "scanner-new-request");
    applyError(timeoutError_, "scanner-new-timeout");
    applyError(concurrencyError_, "scanner-new-concurrency");
    applyError(throttleError_, "scanner-new-throttle");
    applyError(moduleCapError_, "scanner-new-module-cap");
    applyError(modulesError_, "scanner-new-modules");
    if (form_.valid()) {
        summaryLabel_->setVisible(false);
    } else {
        summaryLabel_->setText(QStringLiteral("%1 field(s) need review.")
            .arg(static_cast<quint64>(form_.errors().size())));
        summaryLabel_->setVisible(true);
    }
}

void QtNewAuditDialog::updateAcceptState()
{
    const bool canSubmit = form_.valid() && !pending_ &&
        controller_->initialized() && !controller_->operation().pending() &&
        requestEditor_->isValid() && !requestEditor_->hasUnappliedPretty();
    startButton_->setEnabled(canSubmit);
    if (!controller_->initialized() && !pending_) {
        noticeLabel_->setText(controller_->operation().pending()
            ? QStringLiteral("Scanner is loading — initialization is running in Task Center.")
            : QStringLiteral("Scanner unavailable — retry Scanner initialization before starting an audit."));
        noticeLabel_->setVisible(true);
    } else if (!pending_ && !controller_->statusMessage().isEmpty()) {
        noticeLabel_->setText(controller_->statusMessage());
        noticeLabel_->setVisible(true);
    } else if (!pending_) {
        noticeLabel_->setVisible(false);
    }
}

void QtNewAuditDialog::keyPressEvent(QKeyEvent* event)
{
    if (event->key() != Qt::Key_Return && event->key() != Qt::Key_Enter) {
        AidaDialog::keyPressEvent(event);
        return;
    }
    QWidget* focused = QApplication::focusWidget();
    const bool textInputActive = qobject_cast<QPlainTextEdit*>(focused) != nullptr ||
        qobject_cast<QLineEdit*>(focused) != nullptr ||
        qobject_cast<QAbstractSpinBox*>(focused) != nullptr;
    if (textInputActive && (event->modifiers() & Qt::ControlModifier) == 0) {
        AidaDialog::keyPressEvent(event);
        return;
    }
    validateForm();
    updateAcceptState();
    if (startButton_->isEnabled()) {
        onAccept();
        return;
    }
    form_.request_first_invalid_focus();
    if (form_.consume_focus_request("scanner-new-url"))
        urlEdit_->setFocus();
    else if (form_.consume_focus_request("scanner-new-request"))
        requestEditor_->setFocus();
    else if (form_.consume_focus_request("scanner-new-modules"))
        modulesView_->setFocus();
}

void QtNewAuditDialog::onAccept()
{
    validateForm();
    updateAcceptState();
    if (!startButton_->isEnabled()) {
        form_.request_first_invalid_focus();
        if (form_.consume_focus_request("scanner-new-url"))
            urlEdit_->setFocus();
        else if (form_.consume_focus_request("scanner-new-request"))
            requestEditor_->setFocus();
        else if (form_.consume_focus_request("scanner-new-modules"))
            modulesView_->setFocus();
        return;
    }
    const std::string url = urlEdit_->text().toStdString();
    const std::string raw = requestEditor_->authority().toStdString();
    std::vector<std::uint8_t> rawBytes(raw.begin(), raw.end());
    aida::burp::active_scanner::audit_config_t config;
    config.scope_only = scopeOnlyCheck_->isChecked();
    config.follow_redirects = followRedirectsCheck_->isChecked();
    config.timeout_ms = timeoutSpin_->value();
    config.max_concurrent_requests = static_cast<std::size_t>(concurrencySpin_->value());
    config.request_throttle_ms = static_cast<std::size_t>(throttleSpin_->value());
    config.per_module_request_cap = static_cast<std::size_t>(moduleCapSpin_->value());
    config.max_concurrent_explicit = true;
    config.request_throttle_explicit = true;
    config.enabled_modules = moduleModel_->enabledIds();
    const bool submitted = controller_->submitAudit(std::move(rawBytes), std::move(url),
        std::move(config), generation_);
    if (!submitted) {
        controller_->setStatusMessage(QStringLiteral(
            "Task Center rejected the audit request; review the active operation and retry."));
        updateAcceptState();
    }
    Q_EMIT auditSubmitted(submitted);
}

}
