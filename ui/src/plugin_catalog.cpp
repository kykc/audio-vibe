#include "plugin_catalog.h"

#include "aip/engine/plugin_module.h"
#include "aip/scanner/scanner.h"

#include <QApplication>
#include <QFileInfo>
#include <QProgressDialog>

#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <string>
#include <utility>

namespace aip::ui {

namespace {

/// The most recent thing the worker had to say, read by the GUI thread on its own schedule.
///
/// A one-slot snapshot rather than a queue, and a poll rather than a queued signal: the dialog
/// only ever displays the *latest* entry, so anything older is not worth carrying, and this keeps
/// the whole cross-thread story to one mutex around two fields.
struct ScanProgressSlot {
    std::mutex mutex;
    std::size_t done = 0;
    std::size_t total = 0;
    QString label;
};

} // namespace

bool PluginCatalog::run(QWidget* parent, const std::vector<std::string>& paths,
                        const QString& title, std::vector<scanner::ScannedModule>& out) {
    out.clear();
    if (paths.empty()) {
        return true;
    }

    QProgressDialog dialog(title, QStringLiteral("Cancel"), 0, static_cast<int>(paths.size()),
                           parent);
    dialog.setWindowTitle(QStringLiteral("Scanning plugins"));
    dialog.setWindowModality(Qt::WindowModal);
    dialog.setMinimumDuration(0);
    dialog.setAutoClose(false);
    dialog.setAutoReset(false);
    dialog.setValue(0);

    ScanProgressSlot slot;
    std::atomic<bool> cancelled{false};

    auto scan = std::async(std::launch::async, [&] {
        scanner::ScanOptions options;
        options.cancelled = &cancelled;
        return scanner::scanModules(
            paths, options,
            [&slot](const scanner::ScannedModule& module, std::size_t done, std::size_t total) {
                const std::lock_guard<std::mutex> lock(slot.mutex);
                slot.done = done;
                slot.total = total;
                slot.label = QFileInfo(QString::fromStdString(module.path)).completeBaseName();
            });
    });

    // Pumping the event loop by hand rather than through a QThread and queued signals. It is the
    // smaller mechanism for a modal dialog with one worker, and it has a property that matters
    // here beyond simplicity: EngineHost's servicing tick is a QTimer, so it keeps firing while a
    // scan runs. A chain therefore goes on being serviced -- and a format change goes on being
    // acted upon -- through a scan that may last minutes. Blocking the GUI thread outright would
    // stall the control plane for the whole of it.
    while (scan.wait_for(std::chrono::milliseconds(50)) != std::future_status::ready) {
        if (dialog.wasCanceled()) {
            cancelled.store(true, std::memory_order_relaxed);
        }
        std::size_t done = 0;
        QString label;
        {
            const std::lock_guard<std::mutex> lock(slot.mutex);
            done = slot.done;
            label = slot.label;
        }
        if (!label.isEmpty()) {
            dialog.setValue(static_cast<int>(done));
            dialog.setLabelText(QStringLiteral("%1\n\n%2 of %3")
                                    .arg(label)
                                    .arg(done)
                                    .arg(paths.size()));
        }
        QApplication::processEvents(QEventLoop::AllEvents, 40);
    }

    scanner::ScanReport report = scan.get();
    out = std::move(report.modules);
    // A cancelled scan keeps what it managed to probe -- the entries are real -- and leaves the
    // rest marked NotProbed, which the picker shows as such rather than as broken.
    return !cancelled.load(std::memory_order_relaxed);
}

bool PluginCatalog::ensureScanned(QWidget* parent) {
    if (!modules_.empty()) {
        return true;
    }
    return rescan(parent);
}

bool PluginCatalog::rescan(QWidget* parent) {
    // The directory walk is cheap and loads nothing; only what comes after it needs a child
    // process (see PluginModule::installedModulePaths).
    const std::vector<std::string> paths = engine::PluginModule::installedModulePaths();
    return run(parent, paths, QStringLiteral("Probing installed plugins..."), modules_);
}

scanner::ScannedModule PluginCatalog::probeOne(QWidget* parent, const QString& path) {
    std::vector<scanner::ScannedModule> probed;
    const std::string narrowed = path.toStdString();
    run(parent, {narrowed}, QStringLiteral("Probing %1...").arg(QFileInfo(path).fileName()),
        probed);

    if (probed.empty()) {
        scanner::ScannedModule failed;
        failed.path = narrowed;
        failed.status = scanner::ScanStatus::NotProbed;
        failed.error = "the scan was cancelled";
        return failed;
    }

    // Replace rather than append: browsing to something already in the list should refresh it,
    // not double it.
    for (scanner::ScannedModule& existing : modules_) {
        if (existing.path == probed.front().path) {
            existing = probed.front();
            return existing;
        }
    }
    modules_.push_back(probed.front());
    return modules_.back();
}

QString PluginCatalog::summary() const {
    std::size_t usable = 0;
    for (const scanner::ScannedModule& module : modules_) {
        if (module.usable()) {
            ++usable;
        }
    }
    const std::size_t unusable = modules_.size() - usable;
    if (unusable == 0) {
        return QStringLiteral("%1 plugin(s).").arg(usable);
    }
    return QStringLiteral("%1 plugin(s), %2 unusable.").arg(usable).arg(unusable);
}

} // namespace aip::ui
