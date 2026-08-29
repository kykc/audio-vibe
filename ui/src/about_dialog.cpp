#include "about_dialog.h"

#include "window_chrome.h"

#include "aip/version.h"

#include <QDialog>
#include <QDialogButtonBox>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>
#include <QWidget>

namespace aip::ui {

namespace {

/// A label carrying prose rather than a caption: it wraps, its text can be taken out with the
/// mouse, and any link in it opens in the user's browser.
///
/// Selectable is the load-bearing one. The version line is here so that it can be pasted into a
/// defect report, and a number a user has to retype by hand is a number that arrives wrong.
QLabel* prose(QWidget* parent, const QString& text) {
    auto* label = new QLabel(text, parent);
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::LinksAccessibleByMouse);
    label->setOpenExternalLinks(true);
    return label;
}

QFrame* separator(QWidget* parent) {
    auto* line = new QFrame(parent);
    line->setFrameShape(QFrame::HLine);
    line->setFrameShadow(QFrame::Sunken);
    return line;
}

} // namespace

void showAboutDialog(QWidget* parent) {
    QDialog dialog(parent);
    // Its own title, not the shell's. A dialog that repeated the application name would be a
    // dialog the user cannot name when describing what they were looking at.
    dialog.setWindowTitle(QStringLiteral("About VibeAudio"));
    // No context-help button in the caption; there is no context help.
    dialog.setWindowFlags(dialog.windowFlags() & ~Qt::WindowContextHelpButtonHint);

    auto* layout = new QVBoxLayout(&dialog);

    auto* heading = new QHBoxLayout();

    // 64 is one of the sizes in the icon group rather than a scale of the 256 -- `applicationIcon`
    // asks Windows for each size separately for exactly this reason (window_chrome.h). And it
    // comes out of the running executable's own resources, so there is still only one copy of the
    // picture in the project (design_doc.md sec. 5.6).
    auto* icon = new QLabel(&dialog);
    icon->setPixmap(applicationIcon().pixmap(64, 64));
    icon->setAlignment(Qt::AlignTop);
    heading->addWidget(icon);

    auto* identity = new QVBoxLayout();

    auto* name = new QLabel(QStringLiteral("VibeAudio"), &dialog);
    QFont nameFont = name->font();
    nameFont.setPointSize(nameFont.pointSize() + 6);
    nameFont.setBold(true);
    name->setFont(nameFont);
    identity->addWidget(name);

    // The project version and the commit it was built from, in one line. The commit is resolved
    // at build time (cmake/version.cmake), so this names the binary in front of the user rather
    // than whatever was checked out when somebody last ran CMake.
    identity->addWidget(prose(&dialog,
        QStringLiteral("Version %1 (%2)")
            .arg(QString::fromLatin1(kVersionNumber), QString::fromLatin1(kGitDescription))));

    heading->addLayout(identity, 1);
    // Both columns to the top, and no stretch inside either: a stretch here would be free
    // vertical space that `adjustSize` then bakes into the dialog's height, leaving a band of
    // nothing between the name and the first paragraph.
    heading->setAlignment(Qt::AlignTop);
    layout->addLayout(heading);

    layout->addWidget(separator(&dialog));

    layout->addWidget(prose(&dialog,
        QStringLiteral("System-wide audio processing for Windows: a VST3 plugin chain applied to "
                       "everything an audio device plays, on the audio engine's own clock.")));

    layout->addWidget(separator(&dialog));

    layout->addWidget(prose(&dialog, QStringLiteral("VibeAudio is released under the MIT license.")));

    // The obligations, not a credits roll. Qt is the one that actually imposes something -- it is
    // used under the LGPLv3, which is why it is linked dynamically and its DLLs ship beside the
    // executable rather than inside it (design_doc.md sec. 5.2). The other two are MIT and are
    // named because MIT asks for the notice to travel with the software.
    layout->addWidget(prose(&dialog,
        QStringLiteral("Built with Qt 6 (LGPLv3), the Steinberg VST3 SDK 3.8.1 (MIT) and "
                       "yaml-cpp (MIT). VST is a trademark of Steinberg Media Technologies GmbH.")));

    layout->addWidget(prose(&dialog,
        QStringLiteral("Application icon: \"mixing table\" from "
                       "<a href=\"https://www.flaticon.com/free-icon/mixing-table_5903291\">Flaticon</a>.")));

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    layout->addWidget(buttons);

    // A width, before the size is asked for. Every paragraph here wraps, and a wrapping label
    // will happily report a minimum width of one word -- so a layout made only of them collapses
    // into a column barely wider than the icon and several times taller than it needs to be.
    // Nothing in the dialog can push back on that, because nothing in it has a natural width.
    dialog.setMinimumWidth(460);
    dialog.adjustSize();

    dialog.exec();
}

} // namespace aip::ui
