// The client process (design_doc.md sec. 7.2): one process hosting the GUI, the VST3 plugins,
// their editors and the valet thread. Plugin editors need a UI thread in the same process as their
// HWND, so there is nothing to gain by splitting any of it apart.
//
// The command line exists for verification rather than for daily use -- a shell whose only route
// into a given state is a sequence of mouse clicks cannot be checked without a person:
//
//   vibeaudio                                 the saved session, detached
//   vibeaudio --vst3 <path.vst3>              load into the rack at startup; repeatable
//   vibeaudio --vst3 <path.vst3?Class Name>   and say which effect in it, for a bundle with several
//   vibeaudio --vst3 <path> --editors         and open each one's editor
//   vibeaudio --vst3 <path> --attach          and attach to the default render endpoint
//   vibeaudio --config <path.yaml>            use this session file instead of the usual two
//   vibeaudio --scan                          bring the plugin catalog up to date and report
//
// The `?<class>` suffix is there because a `.vst3` is a module, not a plugin: `lsp-plugins.vst3`
// holds a mono effect and a stereo one, and the full LSP distribution is one bundle holding
// dozens. Without it the engine takes whichever class it can run -- but nothing is attached this
// early, so at start-up there is no stream width for it to judge one by, and the first class wins
// by default. The suffix takes a class name as the picker shows it, or a class id; `?` and no
// other character, because it is one Windows forbids in a path outright
// (engine/plugin_module.h). Quote the whole thing -- names have spaces in them.
//
// `--config` is also the way out of a session that will not load. It is **`--config` and not the
// obvious `--session`** for the same reason as `--vst3` below: `-session` is on Qt's reserved
// list too, alongside `-style`, `-platform`, `-geometry` and `-title`.
//
// Note how the first two combine: the saved session is restored first and `--vst3` appends to it,
// so a plugin named on the command line is in the rack when the shell closes and is therefore
// saved with it. That is the same rule as adding one through the picker -- what is in the rack at
// closing time is what comes back -- but it does mean a one-off verification run leaves a mark.
// Point `--config` somewhere disposable when that is not wanted.
//
// `--attach` is the only one that touches the machine's audio, and it is spelled out for that
// reason: attaching processes every sound on the endpoint, system-wide (sec. 3.7.1).
//
// It is `--vst3` and not the obvious `--plugin` because **`-plugin` is a reserved Qt option**.
// `QGuiApplication` consumes it out of `argv` before any QCommandLineParser runs, and tries to
// load its value as a *Qt* plugin. The symptom is not a diagnostic of any kind: the argument
// simply is not in `QCoreApplication::arguments()`, so the parser reports zero values and the
// shell starts with an empty rack as though nothing had been asked for.

#include "main_window.h"
#include "window_chrome.h"

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>

#include <objbase.h>

int main(int argc, char** argv) {
    // OLE, not just COM, and before Qt: the GUI thread must be a single-threaded apartment,
    // because that is what hosting foreign COM UI in a child HWND requires -- and because it is
    // what `MMDevice` enumeration and Qt's own drag-and-drop expect to find. Qt initialises OLE
    // itself, but doing it here makes the requirement explicit rather than inherited, and nested
    // initialisation is reference-counted.
    const HRESULT ole = ::OleInitialize(nullptr);

    int result = 0;
    {
        QApplication app(argc, argv);
        QApplication::setApplicationName(QStringLiteral("VibeAudio"));
        // Every window this process opens, in one line: Qt gives its own icon to any top-level
        // window that has not set one, so the shell, the picker, the progress dialogs and every
        // message box are all identified the same way (design_doc.md sec. 5.6). A window added
        // later is covered by having been added. The plugin editors are the stated exemption and
        // take their icon off again in their own constructors.
        QApplication::setWindowIcon(aip::ui::applicationIcon());

        QCommandLineParser parser;
        parser.setApplicationDescription(QStringLiteral("System-wide audio processing utility -- client shell."));
        parser.addHelpOption();
        // Not "plugin": see the note at the top of this file. Qt would eat it.
        const QCommandLineOption pluginOption(QStringLiteral("vst3"),
            QStringLiteral("Load a .vst3 into the rack at startup, optionally as <path>?<class>."
                           " Repeatable."),
            QStringLiteral("path"));
        const QCommandLineOption editorsOption(
            QStringLiteral("editors"), QStringLiteral("Open an editor for every loaded plugin."));
        const QCommandLineOption attachOption(
            QStringLiteral("attach"), QStringLiteral("Attach to the default render endpoint at startup."));
        // Not "session": Qt reserves that one as well.
        const QCommandLineOption configOption(QStringLiteral("config"),
            QStringLiteral("Session file to use instead of the portable/AppData search."), QStringLiteral("path"));
        const QCommandLineOption scanOption(
            QStringLiteral("scan"), QStringLiteral("Update the plugin catalog at startup, as the first Add would."));
        parser.addOption(pluginOption);
        parser.addOption(editorsOption);
        parser.addOption(attachOption);
        parser.addOption(configOption);
        parser.addOption(scanOption);
        parser.process(app);

        aip::ui::MainWindow window(parser.value(configOption));
        window.show();
        window.applyStartupOptions(parser.values(pluginOption), parser.isSet(editorsOption), parser.isSet(attachOption),
            parser.isSet(scanOption));
        result = QApplication::exec();
    }

    if (SUCCEEDED(ole)) {
        ::OleUninitialize();
    }
    return result;
}
