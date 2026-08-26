#pragma once

#include <QObject>

#include <cstdint>
#include <string>
#include <string_view>

#include "core/network/network_view.hpp"

namespace aida::qt::net {

// FuzzerController owns the network fuzzer worker machinery moved verbatim
// out of network_view.cpp (the long_running executor loop, the cv handshake,
// run_fuzzer_thread, the results paging/retention helpers and the task-center
// terminal update; network_view.cpp:4157-4827 pre-migration). The state
// fields stay in network_view::state_t (g_state) because the retention pages
// and configuration are shared network-domain state consumed by the artifact
// backends; the controller is the single owner of the worker lifecycle
// (replaces the start_fuzzer_worker spawn in network_view::initialize and the
// join in network_view::shutdown).
//
// Worker->GUI delivery: after every publish_fuzzer_results_locked the worker
// posts a queued functor to the controller
// (QMetaObject::invokeMethod Qt::QueuedConnection; deep-copies the argument
// qmetaobject.cpp:1642-1657; dropped if the controller is destroyed
// qobject.cpp:201-202) which emits resultsPublished on the GUI thread. The
// controller never blocks the GUI thread: shutdownWorker mirrors the legacy
// bounded 2500 ms wait_done loop from network_view::shutdown.
class FuzzerController : public QObject {
    Q_OBJECT
public:
    explicit FuzzerController(QObject* parent = nullptr);
    ~FuzzerController() override;

    static FuzzerController* instance() noexcept { return instance_; }

    bool startWorker();
    void shutdownWorker();
    void clearResults();
    // The verbatim start-of-run critical section (network_view.cpp render
    // fuzzer start handler): under fuzz_mutex, clear the retained results and
    // snapshot the config + task identity for the worker.
    void beginRun(const network_view::state_t::fuzzer_entry_t& config,
                  const std::string& taskId);
    void rejectRun();

    bool workerAvailable() const;

Q_SIGNALS:
    void resultsPublished(quint64 generation);
    void workerStateChanged();

private:
    static void setInstance(FuzzerController* controller) { instance_ = controller; }
    friend void fuzzer_controller_start();
    friend void fuzzer_controller_shutdown();

    static inline FuzzerController* instance_ = nullptr;
};

// Called from network_view::initialize() (GUI thread): creates the singleton
// controller and starts the worker, replacing start_fuzzer_worker.
void fuzzer_controller_start();
// Called from network_view::shutdown(): sets the cancel/alive flags, notifies
// the cv, waits bounded for the worker to finish, then destroys the
// controller (before the Qt object tree dies in the ordered shutdown).
void fuzzer_controller_shutdown();

struct fuzzer_template_shape_t {
    std::string marker;
    std::size_t positions = 0;
    std::string error;
};

fuzzer_template_shape_t analyze_fuzzer_template(std::string_view request);

inline constexpr std::size_t k_fuzzer_page_size = 128;
inline constexpr std::uint64_t k_fuzzer_absolute_request_limit = 1000000;
inline constexpr std::size_t k_fuzzer_payload_set_limit = 64;

}
