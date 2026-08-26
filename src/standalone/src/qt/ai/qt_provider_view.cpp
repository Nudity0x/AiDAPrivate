#include "qt/ai/qt_provider_view.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QList>
#include <QMenu>
#include <QMetaObject>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/ai/provider_catalog.hpp"
#include "core/ai/provider_transforms.hpp"
#include "core/auth/auth_http.hpp"
#include "core/auth/auth_store.hpp"
#include "core/infra/event_bus.hpp"
#include "core/infra/executor.hpp"
#include "core/settings/settings_persistence_service.hpp"
#include "core/settings/standalone_settings.hpp"
#include "core/ui/task_center.hpp"
#include "helpers/diag_log.hpp"
#include "qt/ai/qt_ai_domain.hpp"
#include "qt/bridge/clipboard.hpp"
#include "qt/chrome/aida_toast.hpp"
#include "qt/theme/aida_fonts.hpp"
#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_paint_utils.hpp"

extern settings_sa_t g_sa_settings;

namespace aida::qt::ai {

std::mutex AidaProviderView::s_mtx;
std::map<std::string, AidaProviderView::test_result_t> AidaProviderView::s_pending_results;
std::map<std::string, std::shared_ptr<std::atomic<bool>>> AidaProviderView::s_in_flight_tests;
std::atomic<bool> AidaProviderView::s_refresh_in_flight{false};
std::atomic<bool> AidaProviderView::s_refresh_completed{false};
std::atomic<bool> AidaProviderView::s_refresh_success{false};
std::string AidaProviderView::s_refresh_message;
std::atomic<bool> AidaProviderView::s_shutdown{false};
bool AidaProviderView::s_catalog_load_started = false;

namespace {

using catalog_provider_t = aida::provider::provider_info_t;
using catalog_model_t = aida::provider::model_info_t;
using auth_info_t = aida::auth::auth_info_t;
using auth_kind_t = aida::auth::auth_kind_t;

std::mutex& live_views_mtx() {
    static std::mutex m;
    return m;
}

QList<QPointer<AidaProviderView>>& live_views() {
    static QList<QPointer<AidaProviderView>> views;
    return views;
}

void notify_live_views(void (AidaProviderView::*slot)()) {
    QList<QPointer<AidaProviderView>> targets;
    {
        std::lock_guard<std::mutex> lk(live_views_mtx());
        targets = live_views();
    }
    for (const auto& view : targets) {
        if (view != nullptr)
            QMetaObject::invokeMethod(view, slot, Qt::QueuedConnection);
    }
}

bool submit_provider_view_task(const char* label, aida::infra::executor::domain_t domain,
                               std::function<void()> body) {
    aida::infra::executor::submission_t sub;
    sub.owner_subsystem = "ai_provider_view";
    sub.label = label;
    sub.thread_class = "bounded_task";
    sub.domain = domain;
    sub.priority = 3;
    sub.body = std::move(body);
    const auto submitted = aida::infra::executor::submit(std::move(sub));
    if (submitted.submitted && submitted.task_id != 0) {
        aida::ui::task_center::task_registration_t registration;
        registration.owner = "automation.providers";
        registration.owner_view = "view.ai.providers";
        registration.owner_action = label ? label : "provider.task";
        registration.label = label ? label : "Provider task";
        registration.stage = "Running provider operation";
        registration.cancellation_is_safe = false;
        registration.callbacks.focus = [] {
            open_ai_view("view.ai.providers");
        };
        (void)aida::ui::task_center::register_executor_job(submitted.task_id,
            std::move(registration));
    }
    return submitted.submitted;
}

QString format_cost_pair(double in_per_m, double out_per_m) {
    char buf[96];
    if (in_per_m <= 0.0 && out_per_m <= 0.0)
        std::snprintf(buf, sizeof(buf), "free");
    else
        std::snprintf(buf, sizeof(buf), "$%.2f / $%.2f per M", in_per_m, out_per_m);
    return QString::fromLatin1(buf);
}

QString format_context(int64_t context) {
    if (context <= 0)
        return QStringLiteral("ctx ?");
    char buf[32];
    if (context >= 1000)
        std::snprintf(buf, sizeof(buf), "ctx %lldK", static_cast<long long>(context / 1000));
    else
        std::snprintf(buf, sizeof(buf), "ctx %lld", static_cast<long long>(context));
    return QString::fromLatin1(buf);
}

bool has_auth_for(const std::string& provider_id, auth_info_t& out) {
    if (!aida::auth::store::get(provider_id, out))
        return false;
    if (out.kind == auth_kind_t::none)
        return false;
    return true;
}

struct status_summary_t {
    QString label;
    const char* variant;
};

status_summary_t status_for(const std::string& provider_id) {
    status_summary_t s;
    auth_info_t info;
    const bool present = aida::auth::store::get(provider_id, info);
    if (!present || info.kind == auth_kind_t::none) {
        s.label = QStringLiteral("Not configured");
        s.variant = "neutral";
        return s;
    }
    const auto now = static_cast<int64_t>(std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    if (info.kind == auth_kind_t::oauth) {
        if (info.expires_unix > 0 && info.expires_unix <= now) {
            s.label = QStringLiteral("Token expired");
            s.variant = "warning";
            return s;
        }
        s.label = QStringLiteral("OAuth");
    } else if (info.kind == auth_kind_t::api) {
        s.label = QStringLiteral("API key");
    } else if (info.kind == auth_kind_t::wellknown) {
        s.label = QStringLiteral("Well-known");
    } else {
        s.label = QStringLiteral("Authenticated");
    }
    s.variant = "success";
    return s;
}

void set_label_variant(QLabel* label, const char* variant) {
    if (label->property("aidaVariant") == QLatin1String(variant))
        return;
    label->setProperty("aidaVariant", QString::fromLatin1(variant));
    label->style()->unpolish(label);
    label->style()->polish(label);
}

std::vector<const catalog_model_t*> collect_models_sorted(const std::string& provider_id) {
    std::vector<const catalog_model_t*> out;
    const auto* prov = aida::provider::catalog::get_provider(provider_id);
    if (!prov)
        return out;
    out.reserve(prov->model_ids.size());
    for (const auto& id : prov->model_ids) {
        const auto* m = aida::provider::catalog::get_model(provider_id, id);
        if (m)
            out.push_back(m);
    }
    std::sort(out.begin(), out.end(), [](const catalog_model_t* a, const catalog_model_t* b) {
        const double ca = a->cost.input_per_million + a->cost.output_per_million;
        const double cb = b->cost.input_per_million + b->cost.output_per_million;
        if (ca != cb)
            return ca < cb;
        return a->id < b->id;
    });
    return out;
}

std::string preferred_model_for(const std::string& provider_id) {
    auto& prefs = g_sa_settings.preferred_model_per_provider;
    auto it = prefs.find(provider_id);
    if (it != prefs.end() && !it->second.empty()) {
        if (aida::provider::catalog::get_model(provider_id, it->second) != nullptr)
            return it->second;
    }
    const auto* def = aida::provider::catalog::default_model(provider_id);
    if (def)
        return def->id;
    return std::string();
}

std::string base_url_for(const std::string& provider_id) {
    const auto& overrides = g_sa_settings.provider_base_url_overrides;
    auto it = overrides.find(provider_id);
    if (it != overrides.end() && !it->second.empty())
        return it->second;
    const auto* p = aida::provider::catalog::get_provider(provider_id);
    if (p)
        return p->base_url;
    return std::string();
}

std::string headers_override_for(const std::string& provider_id) {
    const auto& overrides = g_sa_settings.provider_headers_overrides;
    auto it = overrides.find(provider_id);
    if (it != overrides.end())
        return it->second;
    return std::string("{}");
}

std::string raw_model_json_for(const std::string& provider_id, const std::string& model_id) {
    const auto* m = aida::provider::catalog::get_model(provider_id, model_id);
    if (!m)
        return std::string("{}");
    nlohmann::json j;
    j["id"] = m->id;
    j["name"] = m->name;
    j["family"] = m->family;
    j["release_date"] = m->release_date;
    j["status"] = static_cast<int>(m->status);
    j["api"] = { { "id", m->api.id }, { "url", m->api.url }, { "npm", m->api.npm } };
    j["capabilities"] = {
        { "temperature", m->capabilities.temperature },
        { "reasoning", m->capabilities.reasoning },
        { "attachment", m->capabilities.attachment },
        { "tool_call", m->capabilities.tool_call },
        { "interleaved", m->capabilities.interleaved },
        { "input_modalities", m->capabilities.input_modalities },
        { "output_modalities", m->capabilities.output_modalities },
    };
    j["cost"] = {
        { "input_per_million", m->cost.input_per_million },
        { "output_per_million", m->cost.output_per_million },
        { "cache_read_per_million", m->cost.cache_read_per_million },
        { "cache_write_per_million", m->cost.cache_write_per_million },
        { "over_200k_input_per_million", m->cost.over_200k_input_per_million },
        { "over_200k_output_per_million", m->cost.over_200k_output_per_million },
    };
    j["limit"] = {
        { "context", m->limit.context },
        { "input", m->limit.input },
        { "output", m->limit.output },
    };
    j["options"] = m->options;
    j["headers"] = m->headers;
    j["variants"] = m->variants;
    return j.dump(2);
}

bool split_url(const std::string& url, std::string& host_out, std::string& path_out) {
    const size_t scheme_pos = url.find("://");
    if (scheme_pos == std::string::npos) {
        host_out = url;
        path_out = "/";
        return false;
    }
    const size_t host_start = scheme_pos + 3;
    const size_t path_pos = url.find('/', host_start);
    if (path_pos == std::string::npos) {
        host_out = url;
        path_out = "/";
        return true;
    }
    host_out = url.substr(0, path_pos);
    path_out = url.substr(path_pos);
    if (path_out.empty())
        path_out = "/";
    return true;
}

std::string trim_trailing_slash(std::string s) {
    while (!s.empty() && s.back() == '/')
        s.pop_back();
    return s;
}

nlohmann::json build_test_body(const std::string& provider_id, const std::string& model_id) {
    nlohmann::json body;
    if (provider_id == "anthropic") {
        body["model"] = model_id;
        body["max_tokens"] = 1;
        nlohmann::json msg;
        msg["role"] = "user";
        msg["content"] = "ping";
        body["messages"] = nlohmann::json::array({ msg });
        return body;
    }
    if (provider_id == "google" || provider_id == "google-vertex" || provider_id == "vertex") {
        nlohmann::json part;
        part["text"] = "ping";
        nlohmann::json content;
        content["role"] = "user";
        content["parts"] = nlohmann::json::array({ part });
        body["contents"] = nlohmann::json::array({ content });
        body["generationConfig"] = { { "maxOutputTokens", 1 } };
        return body;
    }
    body["model"] = model_id;
    const bool is_o_series =
        model_id.find("o1") != std::string::npos ||
        model_id.find("o3") != std::string::npos ||
        model_id.find("o4") != std::string::npos ||
        model_id.find("o5") != std::string::npos;
    if (is_o_series)
        body["max_completion_tokens"] = 1;
    else
        body["max_tokens"] = 1;
    body["stream"] = false;
    nlohmann::json msg;
    msg["role"] = "user";
    msg["content"] = "ping";
    body["messages"] = nlohmann::json::array({ msg });
    return body;
}

std::string compose_test_path(const std::string& provider_id, const std::string& model_id,
                              const std::string& base_path) {
    if (provider_id == "anthropic") {
        if (base_path.find("/v1/messages") != std::string::npos)
            return base_path;
        std::string p = trim_trailing_slash(base_path);
        if (p.empty() || p == "/")
            return "/v1/messages";
        return p + "/v1/messages";
    }
    if (provider_id == "google" || provider_id == "google-vertex" || provider_id == "vertex") {
        std::string p = trim_trailing_slash(base_path);
        if (p.empty() || p == "/")
            p = "";
        return p + "/v1beta/models/" + model_id + ":generateContent";
    }
    if (base_path.find("/chat/completions") != std::string::npos)
        return base_path;
    if (base_path.find("/responses") != std::string::npos)
        return base_path;
    std::string p = trim_trailing_slash(base_path);
    if (p.empty() || p == "/")
        return "/v1/chat/completions";
    return p + "/v1/chat/completions";
}

double max_total_cost_in_catalog() {
    const auto& providers = aida::provider::catalog::list_providers();
    double max_v = 0.0;
    for (const auto& p : providers) {
        std::string mid = preferred_model_for(p.id);
        if (mid.empty())
            continue;
        const auto* m = aida::provider::catalog::get_model(p.id, mid);
        if (!m)
            continue;
        double total = m->cost.input_per_million + m->cost.output_per_million;
        if (total > max_v)
            max_v = total;
    }
    if (max_v <= 0.0)
        max_v = 1.0;
    return max_v;
}

bool provider_matches_filter(const catalog_provider_t& p, const QString& needle_lower) {
    if (needle_lower.isEmpty())
        return true;
    if (QString::fromStdString(p.id).toLower().contains(needle_lower))
        return true;
    if (QString::fromStdString(p.name).toLower().contains(needle_lower))
        return true;
    for (const auto& mid : p.model_ids) {
        if (QString::fromStdString(mid).toLower().contains(needle_lower))
            return true;
    }
    return false;
}

QColor provider_seed_color(const std::string& seed) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const char c : seed) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 1099511628211ULL;
    }
    return QColor::fromHsl(static_cast<int>(hash % 360ULL), 140, 130);
}

struct test_job_t {
    std::string provider_id;
    std::string model_id;
    std::string key;
    std::shared_ptr<std::atomic<bool>> flag;
};

void finalize_test_result(const std::shared_ptr<test_job_t>& job,
                          const AidaProviderView::test_result_t& result) {
    {
        std::lock_guard<std::mutex> lk(AidaProviderView::s_mtx);
        if (AidaProviderView::s_shutdown.load()) {
            if (job->flag)
                job->flag->store(false);
            AidaProviderView::s_in_flight_tests.erase(job->key);
            AidaProviderView::s_pending_results.erase(job->key);
            return;
        }
        AidaProviderView::s_pending_results[job->key] = result;
        auto fit = AidaProviderView::s_in_flight_tests.find(job->key);
        if (fit != AidaProviderView::s_in_flight_tests.end() && fit->second)
            fit->second->store(false);
    }
    notify_live_views(&AidaProviderView::onTestsUpdated);
}

}

AidaProviderCardWidget::AidaProviderCardWidget(const QString& provider_id, QWidget* parent)
    : QWidget(parent), provider_id_(provider_id) {
    setObjectName(QStringLiteral("aida.ai.provider_card.%1").arg(provider_id));
    const auto& t = theme::tokens();
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(t.panel.padding, t.spacing.sm + t.spacing.xxs,
                             t.panel.padding, t.spacing.sm + t.spacing.xxs);
    root->setSpacing(t.spacing.xs);

    auto* header = new QHBoxLayout();
    header->setSpacing(t.spacing.sm);
    glyph_ = new QLabel(this);
    glyph_->setFixedSize(t.control.toolbar_h, t.control.toolbar_h);
    name_label_ = new QLabel(this);
    name_label_->setFont(theme::fonts::strong());
    status_pill_ = new QLabel(this);
    status_pill_->setFont(theme::fonts::caption());
    header->addWidget(glyph_);
    header->addWidget(name_label_);
    header->addWidget(status_pill_);
    header->addStretch(1);
    badges_label_ = new QLabel(this);
    badges_label_->setFont(theme::fonts::caption());
    badges_label_->setProperty("aidaVariant", QStringLiteral("secondary"));
    header->addWidget(badges_label_);
    root->addLayout(header);

    auto* controls = new QHBoxLayout();
    controls->setSpacing(t.spacing.sm);
    model_combo_ = new QComboBox(this);
    controls->addWidget(model_combo_, 1);
    test_button_ = new QPushButton(QStringLiteral("Test"), this);
    test_button_->setToolTip(QStringLiteral("Send a 1-token ping to verify auth and reachability"));
    default_button_ = new QPushButton(QStringLiteral("Set default"), this);
    default_button_->setToolTip(QStringLiteral("Make this provider's model the default selection"));
    details_button_ = new QPushButton(QStringLiteral("Details"), this);
    details_button_->setToolTip(QStringLiteral("Show endpoint, header, and raw model details"));
    details_button_->setCheckable(true);
    controls->addWidget(test_button_);
    controls->addWidget(default_button_);
    controls->addWidget(details_button_);
    root->addLayout(controls);

    auto* info_row = new QHBoxLayout();
    info_row->setSpacing(t.spacing.sm);
    cost_label_ = new QLabel(this);
    cost_label_->setFont(theme::fonts::caption());
    context_label_ = new QLabel(this);
    context_label_->setFont(theme::fonts::caption());
    cost_bar_ = new QProgressBar(this);
    cost_bar_->setRange(0, 1000);
    cost_bar_->setTextVisible(false);
    cost_bar_->setFixedHeight(t.spacing.xs);
    cost_bar_->setToolTip(QStringLiteral(
        "Combined per-million-token cost relative to the most expensive catalog model"));
    info_row->addWidget(cost_label_);
    info_row->addWidget(context_label_);
    info_row->addStretch(1);
    root->addLayout(info_row);
    root->addWidget(cost_bar_);

    test_result_ = new QLabel(this);
    test_result_->setFont(theme::fonts::caption());
    test_result_->setVisible(false);
    root->addWidget(test_result_);

    connect(model_combo_, &QComboBox::currentIndexChanged, this, [this](int) {
        if (loading_combo_)
            return;
        const QString model_id = model_combo_->currentData().toString();
        if (!model_id.isEmpty())
            Q_EMIT modelSelectionChanged(provider_id_, model_id);
    });
    connect(test_button_, &QPushButton::clicked, this, [this] {
        const QString model_id = model_combo_->currentData().toString();
        if (!model_id.isEmpty())
            Q_EMIT testRequested(provider_id_, model_id);
    });
    connect(default_button_, &QPushButton::clicked, this, [this] {
        const QString model_id = model_combo_->currentData().toString();
        Q_EMIT setDefaultRequested(provider_id_, model_id);
    });
    connect(details_button_, &QPushButton::toggled, this, [this](bool checked) {
        detail_open_ = checked;
        details_button_->setText(checked ? QStringLiteral("Hide") : QStringLiteral("Details"));
        Q_EMIT detailsToggled(provider_id_, checked);
    });
    refreshCard();
}

void AidaProviderCardWidget::contextMenuEvent(QContextMenuEvent* event) {
    Q_EMIT contextMenuRequested(provider_id_, event->globalPos());
    event->accept();
}

void AidaProviderCardWidget::setDetailOpen(bool open) {
    if (detail_open_ == open)
        return;
    detail_open_ = open;
    details_button_->setChecked(open);
    details_button_->setText(open ? QStringLiteral("Hide") : QStringLiteral("Details"));
}

void AidaProviderCardWidget::rebuildModelCombo() {
    loading_combo_ = true;
    model_combo_->clear();
    const std::string pid = provider_id_.toStdString();
    const std::string current = preferred_model_for(pid);
    int current_index = -1;
    const auto models = collect_models_sorted(pid);
    for (const auto* m : models) {
        const QString label = QStringLiteral("%1  -  %2")
            .arg(QString::fromStdString(m->name),
                 format_cost_pair(m->cost.input_per_million, m->cost.output_per_million));
        model_combo_->addItem(label, QString::fromStdString(m->id));
        if (m->id == current)
            current_index = model_combo_->count() - 1;
    }
    if (current_index >= 0)
        model_combo_->setCurrentIndex(current_index);
    loading_combo_ = false;
}

void AidaProviderCardWidget::refreshCard() {
    const auto& t = theme::tokens();
    const std::string pid = provider_id_.toStdString();
    const auto* prov = aida::provider::catalog::get_provider(pid);
    const QString display_name = prov && !prov->name.empty()
        ? QString::fromStdString(prov->name) : provider_id_;
    name_label_->setText(display_name);
    const QColor seed = provider_seed_color(pid);
    const int side = t.control.toolbar_h;
    const qreal dpr = devicePixelRatioF();
    QPixmap pixmap(QSize(side, side) * dpr);
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);
    {
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setBrush(seed);
        painter.setPen(Qt::NoPen);
        painter.drawEllipse(0, 0, side, side);
        painter.setPen(widgets::with_alpha(t.text_primary, 0.90));
        painter.setFont(theme::fonts::ui(700, t.spacing.lg));
        painter.drawText(QRect(0, 0, side, side), Qt::AlignCenter,
            display_name.isEmpty() ? QStringLiteral("?") : display_name.left(1).toUpper());
    }
    glyph_->setPixmap(pixmap);

    const auto status = status_for(pid);
    status_pill_->setText(status.label);
    set_label_variant(status_pill_, status.variant);

    const std::string current_id = preferred_model_for(pid);
    const auto* current_model = current_id.empty()
        ? nullptr : aida::provider::catalog::get_model(pid, current_id);
    if (current_model != nullptr) {
        QStringList badges;
        if (current_model->capabilities.temperature) badges << QStringLiteral("temp");
        if (current_model->capabilities.reasoning) badges << QStringLiteral("reason");
        if (current_model->capabilities.attachment) badges << QStringLiteral("attach");
        if (current_model->capabilities.tool_call) badges << QStringLiteral("tools");
        if (current_model->capabilities.interleaved) badges << QStringLiteral("inter");
        badges_label_->setText(badges.join(QStringLiteral(" ")));
        cost_label_->setText(QStringLiteral("cost: %1")
            .arg(format_cost_pair(current_model->cost.input_per_million,
                                  current_model->cost.output_per_million)));
        context_label_->setText(QStringLiteral("ctx: %1")
            .arg(format_context(current_model->limit.context)));
        const double total = current_model->cost.input_per_million +
            current_model->cost.output_per_million;
        const double ratio = total / max_total_cost_in_catalog();
        cost_bar_->setValue(static_cast<int>(std::clamp(ratio, 0.0, 1.0) * 1000.0));
        cost_bar_->setVisible(true);
    } else {
        badges_label_->setText(prov
            ? QStringLiteral("%1 models").arg(static_cast<int>(prov->model_ids.size()))
            : QString());
        cost_label_->clear();
        context_label_->clear();
        cost_bar_->setVisible(false);
    }
    rebuildModelCombo();

    const bool is_default = (g_sa_settings.default_provider_id == pid);
    default_button_->setText(is_default ? QStringLiteral("Default *")
                                        : QStringLiteral("Set default"));
    updateTestPresentation();
}

void AidaProviderCardWidget::updateTestPresentation() {
    const std::string pid = provider_id_.toStdString();
    const std::string current_id = preferred_model_for(pid);
    const std::string key = current_id.empty() ? std::string() : pid + "/" + current_id;
    bool running = false;
    AidaProviderView::test_result_t result;
    bool has_result = false;
    if (!key.empty()) {
        std::lock_guard<std::mutex> lk(AidaProviderView::s_mtx);
        auto fit = AidaProviderView::s_in_flight_tests.find(key);
        if (fit != AidaProviderView::s_in_flight_tests.end() && fit->second)
            running = fit->second->load();
        auto rit = AidaProviderView::s_pending_results.find(key);
        if (rit != AidaProviderView::s_pending_results.end()) {
            result = rit->second;
            has_result = result.completed;
        }
    }
    test_button_->setText(running ? QStringLiteral("Testing") : QStringLiteral("Test"));
    test_button_->setEnabled(!running && !current_id.empty());
    if (has_result) {
        test_result_->setText(result.success
            ? QStringLiteral("OK %1ms - %2").arg(result.latency_ms)
                .arg(QString::fromStdString(result.message))
            : QStringLiteral("FAIL: %1").arg(QString::fromStdString(result.message)));
        set_label_variant(test_result_, result.success ? "success" : "error");
        test_result_->setVisible(true);
    } else {
        test_result_->setVisible(false);
    }
}

AidaProviderView::AidaProviderView(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("aida.view.ai.providers"));
    s_shutdown.store(false);
    buildUi();
    {
        std::lock_guard<std::mutex> lk(live_views_mtx());
        live_views().append(QPointer<AidaProviderView>(this));
    }
    if (!s_catalog_load_started && aida::provider::catalog::list_providers().empty()) {
        s_catalog_load_started = true;
        (void)submit_provider_view_task("provider_view.load_catalog",
            aida::infra::executor::domain_t::external_tool, [] {
            if (s_shutdown.load())
                return;
            aida::provider::catalog::load_cached_or_fetch(86400);
            notify_live_views(&AidaProviderView::onTestsUpdated);
        });
    }
    rebuildCards();
}

AidaProviderView::~AidaProviderView() {
    std::lock_guard<std::mutex> lk(live_views_mtx());
    live_views().removeAll(QPointer<AidaProviderView>(this));
}

void AidaProviderView::buildUi() {
    const auto& t = theme::tokens();
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(t.spacing.sm + t.spacing.xxs, t.spacing.sm,
                             t.spacing.sm + t.spacing.xxs, t.spacing.sm);
    root->setSpacing(t.spacing.xs + t.spacing.xxs);

    auto* header = new QHBoxLayout();
    header->setSpacing(t.spacing.sm);
    search_edit_ = new QLineEdit(this);
    search_edit_->setObjectName(QStringLiteral("aida.ai.providers.filter"));
    search_edit_->setPlaceholderText(QStringLiteral("Filter providers / models"));
    search_edit_->setClearButtonEnabled(true);
    refresh_button_ = new QPushButton(QStringLiteral("Refresh from models.dev"), this);
    refresh_button_->setObjectName(QStringLiteral("aida.ai.providers.refresh"));
    refresh_button_->setToolTip(QStringLiteral("Fetch the latest provider and model catalog"));
    header->addWidget(search_edit_, 1);
    header->addWidget(refresh_button_);
    root->addLayout(header);

    cache_age_label_ = new QLabel(this);
    cache_age_label_->setFont(theme::fonts::caption());
    cache_age_label_->setProperty("aidaVariant", QStringLiteral("info"));
    cache_age_label_->setVisible(false);
    root->addWidget(cache_age_label_);

    auto* body = new QHBoxLayout();
    body->setSpacing(t.spacing.sm);
    cards_scroll_ = new QScrollArea(this);
    cards_scroll_->setObjectName(QStringLiteral("aida.ai.providers.cards_scroll"));
    cards_scroll_->setWidgetResizable(true);
    cards_host_ = new QWidget();
    cards_layout_ = new QVBoxLayout(cards_host_);
    cards_layout_->setContentsMargins(0, 0, 0, 0);
    cards_layout_->setSpacing(t.spacing.sm);
    cards_layout_->addStretch(1);
    cards_scroll_->setWidget(cards_host_);
    body->addWidget(cards_scroll_, 1);

    detail_pane_ = new QWidget(this);
    detail_pane_->setObjectName(QStringLiteral("aida.ai.providers.detail"));
    detail_pane_->setMinimumWidth(t.row.property_label_w * 2 + t.spacing.lg);
    auto* detail = new QVBoxLayout(detail_pane_);
    detail->setContentsMargins(t.panel.padding, t.panel.padding,
                               t.panel.padding, t.panel.padding);
    detail->setSpacing(t.spacing.xs + t.spacing.xxs);
    detail_title_ = new QLabel(detail_pane_);
    detail_title_->setFont(theme::fonts::h2());
    detail->addWidget(detail_title_);
    auto* base_label = new QLabel(QStringLiteral("Base URL"), detail_pane_);
    base_label->setFont(theme::fonts::bodyEm());
    detail->addWidget(base_label);
    detail_base_url_ = new QLineEdit(detail_pane_);
    detail_base_url_->setPlaceholderText(QStringLiteral("https://api.host"));
    detail->addWidget(detail_base_url_);
    auto* headers_label = new QLabel(QStringLiteral("Extra headers JSON"), detail_pane_);
    headers_label->setFont(theme::fonts::bodyEm());
    detail->addWidget(headers_label);
    detail_headers_ = new QPlainTextEdit(detail_pane_);
    detail_headers_->setFont(theme::fonts::codeRegular());
    detail_headers_->setMaximumBlockCount(256);
    detail_headers_->setFixedHeight(t.control.input_h * 3 + t.spacing.xs);
    detail->addWidget(detail_headers_);
    auto* detail_buttons = new QHBoxLayout();
    auto* save = new QPushButton(QStringLiteral("Save"), detail_pane_);
    auto* reset = new QPushButton(QStringLiteral("Reset"), detail_pane_);
    detail_buttons->addWidget(save);
    detail_buttons->addWidget(reset);
    detail_buttons->addStretch(1);
    detail->addLayout(detail_buttons);
    raw_toggle_ = new QCheckBox(QStringLiteral("Show raw model.json"), detail_pane_);
    detail->addWidget(raw_toggle_);
    detail_raw_json_ = new QPlainTextEdit(detail_pane_);
    detail_raw_json_->setReadOnly(true);
    detail_raw_json_->setFont(theme::fonts::codeRegular());
    detail_raw_json_->setVisible(false);
    detail->addWidget(detail_raw_json_, 1);
    detail_pane_->setVisible(false);
    body->addWidget(detail_pane_);
    root->addLayout(body, 1);

    connect(search_edit_, &QLineEdit::textChanged, this, [this](const QString&) {
        rebuildCards();
    });
    connect(refresh_button_, &QPushButton::clicked, this, [] {
        AidaProviderView::startCatalogRefresh();
    });
    connect(save, &QPushButton::clicked, this, [this] {
        if (selected_detail_provider_id_.isEmpty())
            return;
        const auto pid = selected_detail_provider_id_.toStdString();
        const std::string headers_text = detail_headers_->toPlainText().toStdString();
        auto parsed = nlohmann::json::parse(headers_text, nullptr, false);
        if (parsed.is_discarded() || !parsed.is_object()) {
            chrome::toast_warning(QStringLiteral("Headers JSON invalid - not saved"), 4.0);
            return;
        }
        const std::string base = sa_settings_detail::trim(
            detail_base_url_->text().toStdString());
        if (base.empty())
            g_sa_settings.provider_base_url_overrides.erase(pid);
        else
            g_sa_settings.provider_base_url_overrides[pid] = base;
        g_sa_settings.provider_headers_overrides[pid] = headers_text;
        static_cast<void>(aida::settings_persistence::request_save(g_sa_settings));
        chrome::toast_info(QStringLiteral("Provider details saved"), 3.0);
    });
    connect(reset, &QPushButton::clicked, this, [this] {
        if (selected_detail_provider_id_.isEmpty())
            return;
        const auto pid = selected_detail_provider_id_.toStdString();
        g_sa_settings.provider_base_url_overrides.erase(pid);
        g_sa_settings.provider_headers_overrides.erase(pid);
        static_cast<void>(aida::settings_persistence::request_save(g_sa_settings));
        refreshDetailPane();
        chrome::toast_info(QStringLiteral("Provider overrides cleared"), 3.0);
    });
    connect(raw_toggle_, &QCheckBox::toggled, this, [this](bool checked) {
        detail_raw_json_->setVisible(checked);
        if (checked && !selected_detail_provider_id_.isEmpty()) {
            const auto pid = selected_detail_provider_id_.toStdString();
            detail_raw_json_->setPlainText(QString::fromStdString(
                raw_model_json_for(pid, preferred_model_for(pid))));
        }
    });
    updateCacheAgeCallout();
}

void AidaProviderView::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    updateCacheAgeCallout();
    for (int i = 0; i < cards_layout_->count(); ++i) {
        if (auto* card = qobject_cast<AidaProviderCardWidget*>(
                cards_layout_->itemAt(i)->widget()))
            card->refreshCard();
    }
}

void AidaProviderView::updateCacheAgeCallout() {
    const int64_t age = aida::provider::catalog::cached_age_seconds();
    if (age > 3600) {
        cache_age_label_->setText(QStringLiteral(
            "Provider catalog is cached (%1h old). Refresh to pull the latest models.")
            .arg(age / 3600));
        cache_age_label_->setVisible(true);
    } else {
        cache_age_label_->setVisible(false);
    }
}

void AidaProviderView::rebuildCards() {
    while (QLayoutItem* item = cards_layout_->takeAt(0)) {
        if (QWidget* widget = item->widget())
            widget->deleteLater();
        delete item;
    }
    const QString filter = search_edit_->text().toLower();
    const auto& providers = aida::provider::catalog::list_providers();
    bool any = false;
    for (const auto& provider : providers) {
        if (!provider_matches_filter(provider, filter))
            continue;
        any = true;
        auto* card = new AidaProviderCardWidget(QString::fromStdString(provider.id),
            cards_host_);
        connect(card, &AidaProviderCardWidget::detailsToggled, this,
                [this](const QString& provider_id, bool open) {
            if (open) {
                if (!selected_detail_provider_id_.isEmpty() &&
                    selected_detail_provider_id_ != provider_id) {
                    for (int i = 0; i < cards_layout_->count(); ++i) {
                        if (auto* other = qobject_cast<AidaProviderCardWidget*>(
                                cards_layout_->itemAt(i)->widget())) {
                            if (other->providerId() == selected_detail_provider_id_)
                                other->setDetailOpen(false);
                        }
                    }
                }
                selected_detail_provider_id_ = provider_id;
            } else if (selected_detail_provider_id_ == provider_id) {
                selected_detail_provider_id_.clear();
            }
            refreshDetailPane();
        });
        connect(card, &AidaProviderCardWidget::testRequested, this,
                [](const QString& provider_id, const QString& model_id) {
            AidaProviderView::runTestConnection(provider_id.toStdString(),
                model_id.toStdString());
        });
        connect(card, &AidaProviderCardWidget::setDefaultRequested, this,
                [this](const QString& provider_id, const QString& model_id) {
            if (model_id.isEmpty()) {
                chrome::toast_warning(QStringLiteral("Pick a model first"), 3.0);
                return;
            }
            const auto pid = provider_id.toStdString();
            const auto mid = model_id.toStdString();
            g_sa_settings.set_selection(pid, mid);
            static_cast<void>(aida::settings_persistence::request_save(g_sa_settings));
            aida::events::model_changed_t event;
            event.session_id.clear();
            event.provider_id = pid;
            event.model_id = mid;
            aida::events::publish(aida::events::event_model_changed, event);
            const auto* prov = aida::provider::catalog::get_provider(pid);
            const QString display = prov && !prov->name.empty()
                ? QString::fromStdString(prov->name) : provider_id;
            chrome::toast_info(QStringLiteral("Default set: %1 / %2").arg(display, model_id),
                3.5);
            for (int i = 0; i < cards_layout_->count(); ++i) {
                if (auto* other = qobject_cast<AidaProviderCardWidget*>(
                        cards_layout_->itemAt(i)->widget()))
                    other->refreshCard();
            }
        });
        connect(card, &AidaProviderCardWidget::modelSelectionChanged, this,
                [](const QString& provider_id, const QString& model_id) {
            g_sa_settings.preferred_model_per_provider[provider_id.toStdString()] =
                model_id.toStdString();
            static_cast<void>(aida::settings_persistence::request_save(g_sa_settings));
        });
        connect(card, &AidaProviderCardWidget::contextMenuRequested, this,
                [this](const QString& provider_id, const QPoint& global_pos) {
            const auto pid = provider_id.toStdString();
            const std::string current_model_id = preferred_model_for(pid);
            const bool has_model = !current_model_id.empty() &&
                aida::provider::catalog::get_model(pid, current_model_id) != nullptr;
            const auto validate = [pid, current_model_id]() -> bool {
                const auto* live_provider = aida::provider::catalog::get_provider(pid);
                if (!live_provider)
                    return false;
                if (!current_model_id.empty() &&
                    !aida::provider::catalog::get_model(pid, current_model_id))
                    return false;
                return true;
            };
            auto* menu = new QMenu(this);
            auto* open_details = menu->addAction(QStringLiteral("Open details"));
            auto* set_default = menu->addAction(QStringLiteral("Set as default"));
            set_default->setEnabled(has_model);
            if (!has_model)
                set_default->setToolTip(QStringLiteral(
                    "Choose a provider model before making it the default"));
            auto* test_model = menu->addAction(QStringLiteral("Test connection"));
            test_model->setEnabled(has_model);
            if (!has_model)
                test_model->setToolTip(QStringLiteral(
                    "Choose a provider model before testing the connection"));
            auto* copy_provider = menu->addAction(QStringLiteral("Copy provider id"));
            auto* copy_model = menu->addAction(QStringLiteral("Copy model id"));
            copy_model->setEnabled(has_model);
            connect(open_details, &QAction::triggered, this, [this, validate, provider_id] {
                if (!validate()) {
                    chrome::toast_warning(QStringLiteral(
                        "The provider was removed or replaced; select it again"), 4.0);
                    return;
                }
                selected_detail_provider_id_ = provider_id;
                refreshDetailPane();
            });
            connect(set_default, &QAction::triggered, this, [this, validate, pid,
                                                             current_model_id] {
                if (!validate() || current_model_id.empty())
                    return;
                g_sa_settings.set_selection(pid, current_model_id);
                const auto result = aida::settings_persistence::request_save(g_sa_settings);
                if (!aida::settings_persistence::accepted(result))
                    chrome::toast_error(QStringLiteral(
                        "The default model could not be persisted"), 4.0);
                for (int i = 0; i < cards_layout_->count(); ++i) {
                    if (auto* other = qobject_cast<AidaProviderCardWidget*>(
                            cards_layout_->itemAt(i)->widget()))
                        other->refreshCard();
                }
            });
            connect(test_model, &QAction::triggered, this, [validate, pid,
                                                            current_model_id] {
                if (!validate() || current_model_id.empty())
                    return;
                AidaProviderView::runTestConnection(pid, current_model_id);
            });
            connect(copy_provider, &QAction::triggered, this, [validate, provider_id] {
                if (!validate())
                    return;
                clipboard::set_text(provider_id);
            });
            connect(copy_model, &QAction::triggered, this, [validate, current_model_id] {
                if (!validate() || current_model_id.empty())
                    return;
                clipboard::set_text(QString::fromStdString(current_model_id));
            });
            menu->popup(global_pos);
        });
        cards_layout_->addWidget(card);
    }
    if (!any) {
        auto* empty = new QLabel(QStringLiteral(
            "No providers match the filter. Clear the search or refresh the catalog."),
            cards_host_);
        empty->setFont(theme::fonts::caption());
        empty->setProperty("aidaVariant", QStringLiteral("secondary"));
        empty->setWordWrap(true);
        cards_layout_->addWidget(empty);
    }
    cards_layout_->addStretch(1);
}

void AidaProviderView::refreshDetailPane() {
    if (selected_detail_provider_id_.isEmpty()) {
        detail_pane_->setVisible(false);
        return;
    }
    const auto pid = selected_detail_provider_id_.toStdString();
    const auto* prov = aida::provider::catalog::get_provider(pid);
    if (prov == nullptr) {
        selected_detail_provider_id_.clear();
        detail_pane_->setVisible(false);
        return;
    }
    detail_pane_->setVisible(true);
    detail_title_->setText(QStringLiteral("Details: %1")
        .arg(prov->name.empty() ? QString::fromStdString(prov->id)
                                : QString::fromStdString(prov->name)));
    detail_base_url_->setText(QString::fromStdString(base_url_for(pid)));
    const std::string headers = headers_override_for(pid);
    detail_headers_->setPlainText(QString::fromStdString(
        headers.empty() ? std::string("{}") : headers));
    if (raw_toggle_->isChecked()) {
        detail_raw_json_->setPlainText(QString::fromStdString(
            raw_model_json_for(pid, preferred_model_for(pid))));
    }
}

void AidaProviderView::onRefreshCompleted() {
    if (!s_refresh_completed.exchange(false))
        return;
    if (s_refresh_success.load()) {
        chrome::toast_info(QStringLiteral("Provider catalog refreshed"), 3.0);
    } else {
        std::string message;
        {
            std::lock_guard<std::mutex> lk(s_mtx);
            message = s_refresh_message;
        }
        QString trimmed = QString::fromStdString(message);
        if (trimmed.size() > 200)
            trimmed = trimmed.left(200) + QStringLiteral("...");
        chrome::toast_error(QStringLiteral("Refresh failed: %1").arg(trimmed), 5.0);
    }
    updateCacheAgeCallout();
    rebuildCards();
}

void AidaProviderView::onTestsUpdated() {
    for (int i = 0; i < cards_layout_->count(); ++i) {
        if (auto* card = qobject_cast<AidaProviderCardWidget*>(
                cards_layout_->itemAt(i)->widget()))
            card->refreshCard();
    }
}

void AidaProviderView::startCatalogRefresh() {
    if (s_shutdown.load())
        return;
    bool expected = false;
    if (!s_refresh_in_flight.compare_exchange_strong(expected, true))
        return;
    s_refresh_completed.store(false);
    s_refresh_success.store(false);
    {
        std::lock_guard<std::mutex> lk(s_mtx);
        s_refresh_message.clear();
    }
    const bool posted = submit_provider_view_task("provider_view.refresh_catalog",
        aida::infra::executor::domain_t::external_tool, [] {
        if (s_shutdown.load()) {
            s_refresh_completed.store(false);
            s_refresh_in_flight.store(false);
            return;
        }
        const bool ok = aida::provider::catalog::fetch_and_cache(10000);
        if (s_shutdown.load()) {
            s_refresh_completed.store(false);
            s_refresh_in_flight.store(false);
            return;
        }
        s_refresh_success.store(ok);
        {
            std::lock_guard<std::mutex> lk(s_mtx);
            s_refresh_message = ok ? "Catalog updated"
                : aida::provider::catalog::last_error();
        }
        s_refresh_completed.store(true);
        s_refresh_in_flight.store(false);
        notify_live_views(&AidaProviderView::onRefreshCompleted);
    });
    if (!posted) {
        s_refresh_in_flight.store(false);
        s_refresh_completed.store(false);
    }
}

void AidaProviderView::runTestConnection(const std::string& provider_id,
                                         const std::string& model_id) {
    if (s_shutdown.load())
        return;
    auto job = std::make_shared<test_job_t>();
    job->provider_id = provider_id;
    job->model_id = model_id;
    job->key = provider_id + "/" + model_id;
    {
        std::lock_guard<std::mutex> lk(s_mtx);
        if (s_shutdown.load())
            return;
        auto it = s_in_flight_tests.find(job->key);
        if (it != s_in_flight_tests.end() && it->second && it->second->load())
            return;
        job->flag = std::make_shared<std::atomic<bool>>(true);
        s_in_flight_tests[job->key] = job->flag;
        test_result_t pending;
        pending.provider_id = provider_id;
        pending.model_id = model_id;
        pending.completed = false;
        s_pending_results[job->key] = pending;
    }

    const bool posted = submit_provider_view_task("provider_view.test_connection",
        aida::infra::executor::domain_t::external_tool, [job]() {
        if (s_shutdown.load()) {
            std::lock_guard<std::mutex> lk(s_mtx);
            if (job->flag)
                job->flag->store(false);
            s_in_flight_tests.erase(job->key);
            s_pending_results.erase(job->key);
            return;
        }

        auth_info_t auth_info;
        has_auth_for(job->provider_id, auth_info);
        std::string endpoint = aida::provider::transforms::resolve_endpoint(
            job->provider_id, job->model_id, auth_info);
        std::map<std::string, std::string> headers =
            aida::provider::transforms::compute_headers(job->provider_id, job->model_id,
                auth_info);

        if (s_shutdown.load()) {
            std::lock_guard<std::mutex> lk(s_mtx);
            if (job->flag)
                job->flag->store(false);
            s_in_flight_tests.erase(job->key);
            s_pending_results.erase(job->key);
            return;
        }

        const std::string base_url_override = base_url_for(job->provider_id);
        if (!base_url_override.empty()) {
            std::string ohost;
            std::string opath;
            if (split_url(base_url_override, ohost, opath)) {
                std::string ehost;
                std::string epath;
                if (split_url(endpoint, ehost, epath))
                    endpoint = trim_trailing_slash(ohost) + epath;
                else
                    endpoint = trim_trailing_slash(ohost);
            }
        }

        const std::string headers_json = headers_override_for(job->provider_id);
        if (!headers_json.empty()) {
            auto extra = nlohmann::json::parse(headers_json, nullptr, false);
            if (!extra.is_discarded() && extra.is_object()) {
                for (auto it = extra.begin(); it != extra.end(); ++it) {
                    if (it.value().is_string())
                        headers[it.key()] = it.value().get<std::string>();
                }
            }
        }

        test_result_t result;
        result.provider_id = job->provider_id;
        result.model_id = job->model_id;
        result.completed = true;

        if (endpoint.empty()) {
            result.success = false;
            result.message = "no endpoint resolved (auth or catalog missing)";
            finalize_test_result(job, result);
            return;
        }

        std::string host;
        std::string path;
        if (!split_url(endpoint, host, path)) {
            result.success = false;
            result.message = std::string("malformed endpoint: ") + endpoint;
            finalize_test_result(job, result);
            return;
        }

        if (s_shutdown.load()) {
            std::lock_guard<std::mutex> lk(s_mtx);
            if (job->flag)
                job->flag->store(false);
            s_in_flight_tests.erase(job->key);
            s_pending_results.erase(job->key);
            return;
        }

        path = compose_test_path(job->provider_id, job->model_id, path);

        aida::auth::http::header_list_t test_headers;
        test_headers.reserve(headers.size() + 2);
        test_headers.emplace_back("User-Agent", "AiDAStandalone/1.0");
        test_headers.emplace_back("Accept", "application/json");
        for (const auto& kv : headers)
            test_headers.emplace_back(kv.first, kv.second);

        const nlohmann::json body = build_test_body(job->provider_id, job->model_id);
        const std::string body_str = body.dump();

        std::string post_host = host;
        while (!post_host.empty() && post_host.back() == '/')
            post_host.pop_back();
        const std::string test_url = post_host
            + (path.empty() ? std::string("/")
                : (path.front() == '/' ? path : std::string("/") + path));

        const auto t0 = std::chrono::steady_clock::now();
        aida::auth::http::response_t res = aida::auth::http::post(
            test_url, test_headers, body_str, std::string("application/json"), 20);
        const auto t1 = std::chrono::steady_clock::now();
        result.latency_ms = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count());

        if (s_shutdown.load()) {
            std::lock_guard<std::mutex> lk(s_mtx);
            if (job->flag)
                job->flag->store(false);
            s_in_flight_tests.erase(job->key);
            s_pending_results.erase(job->key);
            return;
        }

        if (!res.ok && res.status == 0) {
            result.success = false;
            result.message = std::string("transport error: ")
                + (res.error.empty() ? std::string("connection failed") : res.error);
        } else {
            result.http_status = res.status;
            if (res.status >= 200 && res.status < 300) {
                result.success = true;
                result.message = std::string("HTTP ") + std::to_string(res.status);
            } else if (res.status == 400 || res.status == 422) {
                result.success = true;
                result.message = std::string("HTTP ") + std::to_string(res.status)
                    + " (auth ok, body rejected)";
            } else if (res.status == 401 || res.status == 403) {
                result.success = false;
                result.message = std::string("HTTP ") + std::to_string(res.status)
                    + " (auth rejected)";
            } else {
                result.success = false;
                std::string snippet = res.body.substr(0, 200);
                result.message = std::string("HTTP ") + std::to_string(res.status) + ": "
                    + snippet;
            }
        }

        finalize_test_result(job, result);
    });

    if (!posted) {
        std::lock_guard<std::mutex> lk(s_mtx);
        if (job->flag)
            job->flag->store(false);
        s_in_flight_tests.erase(job->key);
        s_pending_results.erase(job->key);
    }
    notify_live_views(&AidaProviderView::onTestsUpdated);
}

void AidaProviderView::shutdownWorkers() {
    s_shutdown.store(true);
    std::lock_guard<std::mutex> lk(s_mtx);
    for (auto& kv : s_in_flight_tests) {
        if (kv.second)
            kv.second->store(false);
    }
    s_in_flight_tests.clear();
    s_pending_results.clear();
    s_refresh_in_flight.store(false);
    s_refresh_completed.store(false);
    s_refresh_success.store(false);
    s_refresh_message.clear();
}

}
