// The shell (design_doc.md sec. 5.1, sec. 7.2).
//
// One window, three things in it: which endpoint we are attached to, what is in the rack, and what
// the link is actually doing. The third is not decoration -- protocol v1 gives a client no way to
// ask how it is doing, so the counters are the only evidence that blocks are flowing, that none
// are being dropped, and that the audio thread is still allocation-free (sec. 7.4.3). A shell that
// hid them would make every real problem invisible.
//
// Attaching is a deliberate act, never automatic. Attaching takes over the machine's audio for
// every application at once, and a client that stalls holds up `audiodg.exe` for up to a second
// (sec. 3.7.1) -- so the user asks for it, and the window says what it means.

#pragma once

#include "editor_manager.h"
#include "engine_host.h"
#include "rack_panel.h"

#include "aip/ipc/endpoints.h"

#include <QMainWindow>
#include <QStringList>

#include <vector>

class QComboBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;

namespace aip::ui {

class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    /// Puts the window into the state the command line asked for. Called after `show()`, because
    /// opening an editor needs a shown window to parent it to. Every step reports through the log
    /// view rather than the console -- this is a WIN32 executable and has no console to report to.
    void applyStartupOptions(const QStringList& pluginPaths, bool openEditors, bool attach);

private:
    void refreshEndpoints();
    void toggleAttach();
    void updateStatus();
    void log(const QString& text);

    void onLinkStateChanged(int state, int reason);

    QWidget* buildLinkGroup();
    QWidget* buildRackGroup();
    QWidget* buildCountersGroup();

    // Declaration order is destruction order reversed, and it matters here: the editors must be
    // gone before the engine that owns the plugins behind them. `~MainWindow` closes them by hand
    // rather than trusting that, because Qt deletes child objects after member destruction, which
    // would be far too late.
    EngineHost host_;
    EditorManager editors_;

    std::vector<ipc::RenderEndpoint> endpoints_;

    QComboBox* endpointBox_ = nullptr;
    QPushButton* refreshButton_ = nullptr;
    QPushButton* attachButton_ = nullptr;
    QLabel* linkLabel_ = nullptr;
    RackPanel* rack_ = nullptr;
    QLabel* countersLabel_ = nullptr;
    QPlainTextEdit* logView_ = nullptr;
};

} // namespace aip::ui
