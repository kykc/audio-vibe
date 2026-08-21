#include "editor_manager.h"

#include <QWidget>

#include <vector>

namespace aip::ui {

EditorManager::EditorManager(QObject* parent) : QObject(parent) {}

EditorManager::~EditorManager() { closeAll(); }

bool EditorManager::isOpen(engine::PluginInstance& instance) const {
    return windows_.find(&instance) != windows_.end();
}

void EditorManager::open(engine::PluginInstance& instance, QWidget* parent) {
    const auto existing = windows_.find(&instance);
    if (existing != windows_.end()) {
        existing->second->raise();
        existing->second->activateWindow();
        return;
    }

    QString error;
    EditorWindow* window = EditorWindow::create(instance, parent, error);
    if (window == nullptr) {
        Q_EMIT message(QString::fromStdString(instance.name()) + QStringLiteral(": ") + error);
        return;
    }

    connect(window, &EditorWindow::closed, this, &EditorManager::onWindowClosed);
    windows_.emplace(&instance, window);
    window->raise();
    window->activateWindow();

    Q_EMIT openCountChanged();
    Q_EMIT message(QStringLiteral("editor open: %1 (%2 x %3, %4 child window(s), scale-aware: %5)")
                       .arg(QString::fromStdString(instance.name()))
                       .arg(window->width())
                       .arg(window->height())
                       .arg(window->childWindowCount())
                       .arg(window->scaleAware() ? QStringLiteral("yes") : QStringLiteral("no")));
}

void EditorManager::close(engine::PluginInstance& instance) {
    const auto found = windows_.find(&instance);
    if (found == windows_.end()) {
        return;
    }
    EditorWindow* window = found->second;
    windows_.erase(found);

    // Order matters, and this is the order: disconnect first so the destruction below cannot
    // re-enter through `closed`, then release the plugin's view, then destroy the window. Deleting
    // it outright rather than deferring is deliberate -- the caller is about to destroy the
    // instance, and a deferred delete would run after that.
    window->disconnect(this);
    window->release();
    delete window;
    Q_EMIT openCountChanged();
}

void EditorManager::closeAll() {
    // Copied out first: closing mutates the map.
    std::vector<EditorWindow*> open;
    open.reserve(windows_.size());
    for (const auto& [instance, window] : windows_) {
        open.push_back(window);
    }
    windows_.clear();

    for (EditorWindow* window : open) {
        window->disconnect(this);
        window->release();
        delete window;
    }
}

void EditorManager::onWindowClosed(EditorWindow* window) {
    // The window has already released its view and is about to be deleted by Qt; all that is left
    // is to stop naming it. Searched by window rather than by instance because `release()` has
    // already cleared the window's idea of which instance it belonged to.
    for (auto it = windows_.begin(); it != windows_.end(); ++it) {
        if (it->second == window) {
            windows_.erase(it);
            Q_EMIT openCountChanged();
            return;
        }
    }
}

} // namespace aip::ui
