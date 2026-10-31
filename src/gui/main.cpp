#include "audio/pipewire_engine.hpp"
#include "audio/config.hpp"
#include "audio/pipewire_devices.hpp"
#include "audio/plugin_discovery.hpp"
#include "audio/vst3_processor.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFormLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QAbstractItemView>
#include <QMainWindow>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QPointer>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <X11/Xatom.h>
#include <X11/Xlib.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <future>
#include <thread>
#include <algorithm>
#include <chrono>
#include <vector>

namespace {

class XEmbedSurface final {
public:
    explicit XEmbedSurface(Window qtParent) {
        display_ = XOpenDisplay(nullptr);
        if (!display_) {
            error_ = "Could not open the X11 display for the VST3 editor host.";
            std::fprintf(stderr, "[VST3 XEmbed] XOpenDisplay failed (DISPLAY=%s).\n",
                         std::getenv("DISPLAY") ? std::getenv("DISPLAY") : "<unset>");
            return;
        }
        std::fprintf(stderr, "[VST3 XEmbed] Connected to display '%s'; Qt parent=0x%llx.\n",
                     DisplayString(display_),
                     static_cast<unsigned long long>(qtParent));
        XWindowAttributes parentAttributes{};
        if (!XGetWindowAttributes(display_, qtParent, &parentAttributes)) {
            error_ = "The Qt editor container is not a valid X11 window on this display.";
            std::fprintf(stderr, "[VST3 XEmbed] ERROR: Qt parent window is not valid on this X11 display.\n");
            XCloseDisplay(display_);
            display_ = nullptr;
            return;
        }
        std::fprintf(stderr, "[VST3 XEmbed] Qt parent geometry=%dx%d at (%d,%d).\n",
                     parentAttributes.width, parentAttributes.height,
                     parentAttributes.x, parentAttributes.y);
        XSetWindowAttributes attributes{};
        attributes.override_redirect = True;
        window_ = XCreateWindow(display_, qtParent, 0, 0, 320, 180, 0, CopyFromParent,
                                InputOutput, CopyFromParent, CWOverrideRedirect, &attributes);
        if (!window_) {
            error_ = "Could not create the native X11 editor container.";
            XCloseDisplay(display_);
            display_ = nullptr;
            return;
        }
        XSelectInput(display_, window_, StructureNotifyMask | SubstructureNotifyMask |
                                         PropertyChangeMask | FocusChangeMask | ExposureMask);
        std::fprintf(stderr, "[VST3 XEmbed] Created and selected events on container=0x%llx.\n",
                     static_cast<unsigned long long>(window_));
        xembedAtom_ = XInternAtom(display_, "_XEMBED", False);
        xembedInfoAtom_ = XInternAtom(display_, "_XEMBED_INFO", False);
        XMapWindow(display_, window_);
        XFlush(display_);
        pollTimer_ = new QTimer();
        pollTimer_->setInterval(15);
        QObject::connect(pollTimer_, &QTimer::timeout, [this] { pumpEvents(); });
        pollTimer_->start();
    }

    ~XEmbedSurface() {
        if (pollTimer_) {
            pollTimer_->stop();
            delete pollTimer_;
        }
        if (display_) {
            if (window_) XDestroyWindow(display_, window_);
            XCloseDisplay(display_);
        }
    }

    bool valid() const { return display_ != nullptr && window_ != 0; }
    Window window() const { return window_; }
    const std::string& error() const { return error_; }

    void editorAttached() {
        editorAttached_ = true;
        attachTimer_.start();
        noChildWarningPrinted_ = false;
        std::fprintf(stderr,
                     "[VST3 XEmbed] VST3 attached() returned success; waiting for plugin child-window creation.\n");
    }

    void resizeTo(int width, int height) {
        lastWidth_ = std::max(1, width);
        lastHeight_ = std::max(1, height);
        if (!valid()) return;
        XResizeWindow(display_, window_, static_cast<unsigned int>(lastWidth_),
                      static_cast<unsigned int>(lastHeight_));
        for (const auto child : children_) {
            XMoveResizeWindow(display_, child, 0, 0,
                              static_cast<unsigned int>(lastWidth_),
                              static_cast<unsigned int>(lastHeight_));
        }
        XFlush(display_);
    }

private:
    void sendEmbeddedNotify(Window child) {
        XEvent message{};
        message.xclient.type = ClientMessage;
        message.xclient.display = display_;
        message.xclient.window = child;
        message.xclient.message_type = xembedAtom_;
        message.xclient.format = 32;
        message.xclient.data.l[0] = CurrentTime;
        message.xclient.data.l[1] = 0; // XEMBED_EMBEDDED_NOTIFY
        message.xclient.data.l[2] = 0; // detail
        message.xclient.data.l[3] = static_cast<long>(window_);
        message.xclient.data.l[4] = embedVersion_;
        XSendEvent(display_, child, False, NoEventMask, &message);
        XFlush(display_);
    }

    void sendXEmbedMessage(Window child, long opcode) {
        XEvent message{};
        message.xclient.type = ClientMessage;
        message.xclient.display = display_;
        message.xclient.window = child;
        message.xclient.message_type = xembedAtom_;
        message.xclient.format = 32;
        message.xclient.data.l[0] = CurrentTime;
        message.xclient.data.l[1] = opcode;
        message.xclient.data.l[2] = 0;
        message.xclient.data.l[3] = static_cast<long>(window_);
        message.xclient.data.l[4] = embedVersion_;
        XSendEvent(display_, child, False, NoEventMask, &message);
    }

    void pumpEvents() {
        if (!valid()) return;
        while (XPending(display_) > 0) {
            XEvent event{};
            XNextEvent(display_, &event);
            switch (event.type) {
            case CreateNotify:
                if (event.xcreatewindow.parent == window_) {
                    const Window child = event.xcreatewindow.window;
                    children_.push_back(child);
                    std::fprintf(stderr,
                                 "[VST3 XEmbed] Plugin created child window=0x%llx (override_redirect=%d).\n",
                                 static_cast<unsigned long long>(child),
                                 event.xcreatewindow.override_redirect ? 1 : 0);
                    Atom actualType = None;
                    int actualFormat = 0;
                    unsigned long itemCount = 0;
                    unsigned long bytesAfter = 0;
                    unsigned char* property = nullptr;
                    const auto propertyResult = XGetWindowProperty(
                        display_, child, xembedInfoAtom_, 0, 2, False, xembedInfoAtom_,
                        &actualType, &actualFormat, &itemCount, &bytesAfter, &property);
                    if (propertyResult == Success && property != nullptr &&
                        actualFormat == 32 && itemCount >= 2) {
                        const auto* values = reinterpret_cast<unsigned long*>(property);
                        embedVersion_ = static_cast<long>(values[0]);
                        const auto flags = values[1];
                        std::fprintf(stderr,
                                     "[VST3 XEmbed] _XEMBED_INFO version=%ld flags=0x%lx.\n",
                                     embedVersion_, flags);
                        if ((flags & 1UL) != 0UL) {
                            std::fprintf(stderr,
                                         "[VST3 XEmbed] WARNING: child advertises itself as already mapped before embedding.\n");
                        }
                    } else {
                        std::fprintf(stderr,
                                     "[VST3 XEmbed] WARNING: _XEMBED_INFO unavailable or malformed (result=%d, type=%lu, format=%d, items=%lu).\n",
                                     propertyResult, static_cast<unsigned long>(actualType),
                                     actualFormat, itemCount);
                    }
                    if (property != nullptr) XFree(property);
                    sendEmbeddedNotify(child);
                    XMapWindow(display_, child);
                    XMoveResizeWindow(display_, child, 0, 0,
                                      static_cast<unsigned int>(lastWidth_),
                                      static_cast<unsigned int>(lastHeight_));
                    XSync(display_, False);
                    sendXEmbedMessage(child, 1); // XEMBED_WINDOW_ACTIVATE
                    sendXEmbedMessage(child, 4); // XEMBED_FOCUS_IN
                    XWindowAttributes childAttributes{};
                    if (XGetWindowAttributes(display_, child, &childAttributes)) {
                        std::fprintf(stderr,
                                     "[VST3 XEmbed] Child after map: map_state=%d geometry=%dx%d.\n",
                                     childAttributes.map_state, childAttributes.width,
                                     childAttributes.height);
                    }
                }
                break;
            case MapRequest:
                std::fprintf(stderr, "[VST3 XEmbed] MapRequest for window=0x%llx.\n",
                             static_cast<unsigned long long>(event.xmaprequest.window));
                XMapWindow(display_, event.xmaprequest.window);
                break;
            case MapNotify:
                if (std::find(children_.begin(), children_.end(), event.xmap.window) != children_.end()) {
                    std::fprintf(stderr, "[VST3 XEmbed] Plugin child MapNotify window=0x%llx.\n",
                                 static_cast<unsigned long long>(event.xmap.window));
                }
                break;
            case UnmapNotify:
                if (std::find(children_.begin(), children_.end(), event.xunmap.window) != children_.end()) {
                    std::fprintf(stderr, "[VST3 XEmbed] Plugin child UnmapNotify window=0x%llx.\n",
                                 static_cast<unsigned long long>(event.xunmap.window));
                }
                break;
            case ConfigureRequest: {
                const auto& request = event.xconfigurerequest;
                XWindowChanges changes{};
                changes.x = request.x;
                changes.y = request.y;
                changes.width = request.width;
                changes.height = request.height;
                changes.border_width = request.border_width;
                changes.sibling = request.above;
                changes.stack_mode = request.detail;
                XConfigureWindow(display_, request.window, request.value_mask, &changes);
                break;
            }
            case FocusIn:
                for (const auto child : children_) sendXEmbedMessage(child, 1);
                break;
            case FocusOut:
                for (const auto child : children_) sendXEmbedMessage(child, 2);
                break;
            case DestroyNotify:
                children_.erase(std::remove(children_.begin(), children_.end(),
                                            event.xdestroywindow.window), children_.end());
                break;
            default:
                break;
            }
        }
        if (editorAttached_ && children_.empty() && !noChildWarningPrinted_ &&
            attachTimer_.isValid() && attachTimer_.elapsed() >= 2000) {
            noChildWarningPrinted_ = true;
            std::fprintf(stderr,
                         "[VST3 XEmbed] DIAGNOSTIC: no plugin child X11 window appeared within 2 seconds of attached() success. The plugin created a VST3 view, but did not create an embeddable X11 child window.\n");
        }
        XFlush(display_);
    }

    Display* display_{};
    Window window_{};
    Atom xembedAtom_{};
    Atom xembedInfoAtom_{};
    long embedVersion_{};
    QTimer* pollTimer_{};
    int lastWidth_{320};
    int lastHeight_{180};
    std::vector<Window> children_;
    std::string error_;
    QElapsedTimer attachTimer_;
    bool editorAttached_{false};
    bool noChildWarningPrinted_{false};
};

class PluginEditorDialog final : public QDialog {
public:
    explicit PluginEditorDialog(vst_host::audio::Vst3Processor* processor,
                                QWidget* parent = nullptr)
        : QDialog(parent), processor_(processor) {
        setWindowTitle(tr("VST3 Plugin Editor"));
        auto* layout = new QVBoxLayout(this);
        surface_ = new QWidget(this);
        surface_->setAttribute(Qt::WA_NativeWindow);
        surface_->setMinimumSize(320, 180);
        layout->addWidget(surface_);
        resize(480, 320);
    }

    ~PluginEditorDialog() override {
        if (processor_ != nullptr && attached_) {
            processor_->close_editor();
        }
    }

    bool attachEditor(QString& error) {
        if (processor_ == nullptr) {
            error = tr("The audio engine has not loaded a plugin yet.");
            return false;
        }
        show();
        surface_->show();
        QApplication::processEvents();

        int width = 0;
        int height = 0;
        std::string detail;
        const auto nativeId = static_cast<Window>(surface_->winId());
        xembed_ = std::make_unique<XEmbedSurface>(nativeId);
        if (!xembed_->valid()) {
            error = QString::fromStdString(xembed_->error());
            return false;
        }
        auto* nativeParent = reinterpret_cast<void*>(static_cast<std::uintptr_t>(xembed_->window()));
        if (!processor_->open_editor(nativeParent, width, height, detail)) {
            error = QString::fromStdString(detail);
            return false;
        }
        attached_ = true;
        xembed_->editorAttached();
        surface_->setMinimumSize(width, height);
        surface_->resize(width, height);
        xembed_->resizeTo(width, height);
        resize(width + 24, height + 48);
        return true;
    }

private:
    vst_host::audio::Vst3Processor* processor_{};
    QWidget* surface_{};
    std::unique_ptr<XEmbedSurface> xembed_;
    bool attached_{false};
};

class MainWindow final : public QMainWindow {
public:
    MainWindow() {
        setWindowTitle(tr("VST Host Experiment"));
        resize(760, 620);

        auto* central = new QWidget(this);
        auto* root = new QVBoxLayout(central);
        auto* form = new QFormLayout();

        auto* pluginRow = new QWidget(central);
        auto* pluginLayout = new QHBoxLayout(pluginRow);
        pluginLayout->setContentsMargins(0, 0, 0, 0);
        pluginPath_ = new QLineEdit(pluginRow);
        pluginPath_->setPlaceholderText(tr("Select a native Linux .vst3 bundle"));
        pluginLayout->addWidget(pluginPath_, 1);
        addPluginButton_ = new QPushButton(tr("Add effect"), pluginRow);
        pluginLayout->addWidget(addPluginButton_);
        form->addRow(tr("Plugin bundle"), pluginRow);

        chainList_ = new QListWidget(central);
        chainList_->setSelectionMode(QAbstractItemView::SingleSelection);
        chainList_->setDragDropMode(QAbstractItemView::InternalMove);
        root->addWidget(new QLabel(tr("Processing chain (top to bottom)"), central));
        root->addWidget(chainList_, 1);
        auto* chainActions = new QHBoxLayout();
        removePluginButton_ = new QPushButton(tr("Remove selected"), central);
        moveUpButton_ = new QPushButton(tr("Move up"), central);
        moveDownButton_ = new QPushButton(tr("Move down"), central);
        chainActions->addWidget(removePluginButton_);
        chainActions->addWidget(moveUpButton_);
        chainActions->addWidget(moveDownButton_);
        root->addLayout(chainActions);

        auto* configActions = new QHBoxLayout();
        auto* loadConfigButton = new QPushButton(tr("Load configuration…"), central);
        auto* saveConfigButton = new QPushButton(tr("Save configuration…"), central);
        configActions->addWidget(loadConfigButton);
        configActions->addWidget(saveConfigButton);
        root->addLayout(configActions);
        connect(loadConfigButton, &QPushButton::clicked, this, [this] { loadConfiguration(); });
        connect(saveConfigButton, &QPushButton::clicked, this, [this] { saveConfiguration(); });

        root->addWidget(new QLabel(tr("Plugin search directories"), central));
        pluginDirectories_ = new QListWidget(central);
        pluginDirectories_->setMaximumHeight(90);
        root->addWidget(pluginDirectories_);
        auto* directoryActions = new QHBoxLayout();
        auto* addDirectoryButton = new QPushButton(tr("Add plugin directory…"), central);
        auto* removeDirectoryButton = new QPushButton(tr("Remove directory"), central);
        auto* rescanDirectoriesButton = new QPushButton(tr("Scan directories"), central);
        directoryActions->addWidget(addDirectoryButton);
        directoryActions->addWidget(removeDirectoryButton);
        directoryActions->addWidget(rescanDirectoriesButton);
        root->addLayout(directoryActions);
        connect(addDirectoryButton, &QPushButton::clicked, this, [this] {
            const auto path = QFileDialog::getExistingDirectory(this, tr("Add plugin search directory"));
            if (path.isEmpty()) return;
            bool exists = false;
            for (int i = 0; i < pluginDirectories_->count(); ++i)
                if (pluginDirectories_->item(i)->text() == path) exists = true;
            if (!exists) pluginDirectories_->addItem(path);
            scanPluginDirectories();
            saveSettings();
        });
        connect(removeDirectoryButton, &QPushButton::clicked, this, [this] {
            const int row = pluginDirectories_->currentRow();
            if (row >= 0) delete pluginDirectories_->takeItem(row);
            saveSettings();
        });
        connect(rescanDirectoriesButton, &QPushButton::clicked, this, [this] {
            const auto countBefore = chainList_->count();
            scanPluginDirectories();
            status_->setText(tr("Directory scan complete. %1 new plugin(s) added.")
                                 .arg(chainList_->count() - countBefore));
            saveSettings();
        });

        root->addWidget(new QLabel(tr("Windows VST3 directories (Wine / yabridge)"), central));
        windowsPluginDirectories_ = new QListWidget(central);
        windowsPluginDirectories_->setMaximumHeight(90);
        root->addWidget(windowsPluginDirectories_);
        auto* windowsDirectoryActions = new QHBoxLayout();
        auto* addWindowsDirectoryButton = new QPushButton(tr("Add Windows plugin directory…"), central);
        auto* removeWindowsDirectoryButton = new QPushButton(tr("Remove Windows directory"), central);
        windowsDirectoryActions->addWidget(addWindowsDirectoryButton);
        windowsDirectoryActions->addWidget(removeWindowsDirectoryButton);
        root->addLayout(windowsDirectoryActions);
        connect(addWindowsDirectoryButton, &QPushButton::clicked, this, [this] {
            const auto path = QFileDialog::getExistingDirectory(this, tr("Add Windows VST3 directory"));
            if (path.isEmpty()) return;
            bool exists = false;
            for (int i = 0; i < windowsPluginDirectories_->count(); ++i)
                if (windowsPluginDirectories_->item(i)->text() == path) exists = true;
            if (!exists) windowsPluginDirectories_->addItem(path);
            saveSettings();
        });
        connect(removeWindowsDirectoryButton, &QPushButton::clicked, this, [this] {
            const int row = windowsPluginDirectories_->currentRow();
            if (row >= 0) delete windowsPluginDirectories_->takeItem(row);
            saveSettings();
        });

        auto* inputRow = new QWidget(central);
        auto* inputLayout = new QHBoxLayout(inputRow);
        inputLayout->setContentsMargins(0, 0, 0, 0);
        inputDevice_ = new QComboBox(inputRow);
        inputDevice_->setEditable(true);
        inputDevice_->lineEdit()->setPlaceholderText(tr("Default microphone or manual PipeWire node name"));
        inputLayout->addWidget(inputDevice_, 1);
        auto* refreshDevicesButton = new QPushButton(tr("Refresh"), inputRow);
        inputLayout->addWidget(refreshDevicesButton);
        form->addRow(tr("Microphone / input"), inputRow);
        connect(refreshDevicesButton, &QPushButton::clicked, this, [this] { refreshInputDevices(); });

        outputName_ = new QLineEdit(QStringLiteral("vst-host-virtual-mic"), central);
        form->addRow(tr("Virtual microphone name"), outputName_);

        gain_ = new QDoubleSpinBox(central);
        gain_->setRange(0.0, 4.0);
        gain_->setSingleStep(0.1);
        gain_->setValue(1.0);
        gain_->setDecimals(2);
        form->addRow(tr("Output gain"), gain_);

        latency_ = new QComboBox(central);
        latency_->addItems({QStringLiteral("128"), QStringLiteral("256"),
                            QStringLiteral("512"), QStringLiteral("1024")});
        form->addRow(tr("Latency buffer (frames)"), latency_);

        bypass_ = new QCheckBox(tr("Bypass output gain"), central);
        root->addLayout(form);
        root->addWidget(bypass_);

        auto* actions = new QHBoxLayout();
        startButton_ = new QPushButton(tr("Start audio"), central);
        stopButton_ = new QPushButton(tr("Stop audio"), central);
        editorButton_ = new QPushButton(tr("Open plugin editor"), central);
        stopButton_->setEnabled(false);
        editorButton_->setEnabled(false);
        actions->addWidget(startButton_);
        actions->addWidget(stopButton_);
        actions->addWidget(editorButton_);
        root->addLayout(actions);

        status_ = new QLabel(tr("Ready. Choose a plugin to begin."), central);
        status_->setWordWrap(true);
        root->addWidget(status_);
        diagnostics_ = new QLabel(tr("Audio diagnostics: not running"), central);
        root->addWidget(diagnostics_);
        auto* note = new QLabel(
            tr("Plugin editor embedding currently requires the Qt X11 (xcb) platform. "
               "Wayland-native editor embedding is not implemented yet."), central);
        note->setWordWrap(true);
        root->addWidget(note);
        setCentralWidget(central);

        connect(addPluginButton_, &QPushButton::clicked, this, [this] {
            const auto path = QFileDialog::getExistingDirectory(
                this, tr("Add VST3 effect bundle"), pluginPath_->text());
            if (!path.isEmpty()) {
                chainList_->addItem(path);
                chainList_->setCurrentRow(chainList_->count() - 1);
                pluginPath_->setText(path);
            }
        });
        connect(removePluginButton_, &QPushButton::clicked, this, [this] {
            const int row = chainList_->currentRow();
            if (row >= 0) delete chainList_->takeItem(row);
        });
        connect(moveUpButton_, &QPushButton::clicked, this, [this] {
            moveSelectedPlugin(-1);
        });
        connect(moveDownButton_, &QPushButton::clicked, this, [this] {
            moveSelectedPlugin(1);
        });
        connect(startButton_, &QPushButton::clicked, this, [this] { startAudio(); });
        connect(stopButton_, &QPushButton::clicked, this, [this] { stopAudio(); });
        connect(editorButton_, &QPushButton::clicked, this, [this] { openEditor(); });

        pollTimer_ = new QTimer(this);
        pollTimer_->setInterval(200);
        connect(pollTimer_, &QTimer::timeout, this, [this] { pollEngine(); });
        loadSettings();
        refreshInputDevices();
        pollTimer_->start();
    }

    ~MainWindow() override {
        saveSettings();
        stopAudio();
    }

protected:
    void closeEvent(QCloseEvent* event) override {
        stopAudio();
        QMainWindow::closeEvent(event);
    }

private:
    void loadSettings() {
        QSettings settings(QStringLiteral("VSTHostExperiment"),
                           QStringLiteral("VSTHostExperiment"));
        settings.beginGroup(QStringLiteral("audio"));
        const auto paths = settings.value(QStringLiteral("pluginChain")).toStringList();
        for (const auto& path : paths) chainList_->addItem(path);
        const auto directories = settings.value(QStringLiteral("pluginDirectories")).toStringList();
        for (const auto& directory : directories) pluginDirectories_->addItem(directory);
        const auto windowsDirectories = settings.value(QStringLiteral("windowsPluginDirectories")).toStringList();
        for (const auto& directory : windowsDirectories) windowsPluginDirectories_->addItem(directory);
        inputDevice_->setEditText(settings.value(QStringLiteral("inputTarget")).toString());
        outputName_->setText(settings.value(QStringLiteral("outputName"),
                                             QStringLiteral("vst-host-virtual-mic")).toString());
        gain_->setValue(settings.value(QStringLiteral("gain"), 1.0).toDouble());
        latency_->setCurrentText(settings.value(QStringLiteral("latencyFrames"),
                                                QStringLiteral("128")).toString());
        bypass_->setChecked(settings.value(QStringLiteral("gainBypassed"), false).toBool());
        settings.endGroup();
        scanPluginDirectories();
        if (chainList_->count() > 0) chainList_->setCurrentRow(0);
    }

    void saveSettings() const {
        QSettings settings(QStringLiteral("VSTHostExperiment"),
                           QStringLiteral("VSTHostExperiment"));
        settings.beginGroup(QStringLiteral("audio"));
        QStringList paths;
        for (int i = 0; i < chainList_->count(); ++i)
            paths.push_back(chainList_->item(i)->text());
        settings.setValue(QStringLiteral("pluginChain"), paths);
        QStringList directories;
        for (int i = 0; i < pluginDirectories_->count(); ++i)
            directories.push_back(pluginDirectories_->item(i)->text());
        settings.setValue(QStringLiteral("pluginDirectories"), directories);
        QStringList windowsDirectories;
        for (int i = 0; i < windowsPluginDirectories_->count(); ++i)
            windowsDirectories.push_back(windowsPluginDirectories_->item(i)->text());
        settings.setValue(QStringLiteral("windowsPluginDirectories"), windowsDirectories);
        settings.setValue(QStringLiteral("inputTarget"), inputTargetValue());
        settings.setValue(QStringLiteral("outputName"), outputName_->text());
        settings.setValue(QStringLiteral("gain"), gain_->value());
        settings.setValue(QStringLiteral("latencyFrames"), latency_->currentText());
        settings.setValue(QStringLiteral("gainBypassed"), bypass_->isChecked());
        settings.endGroup();
        settings.sync();
    }

    vst_host::audio::PipeWireOptions optionsFromUi() const {
        vst_host::audio::PipeWireOptions options;
        for (int i = 0; i < chainList_->count(); ++i)
            options.plugin_paths.push_back(chainList_->item(i)->text().toStdString());
        for (int i = 0; i < pluginDirectories_->count(); ++i)
            options.plugin_directories.push_back(pluginDirectories_->item(i)->text().toStdString());
        for (int i = 0; i < windowsPluginDirectories_->count(); ++i)
            options.windows_plugin_directories.push_back(windowsPluginDirectories_->item(i)->text().toStdString());
        options.input_target = inputTargetValue().toStdString();
        options.output_name = outputName_->text().trimmed().toStdString();
        options.output_description = options.output_name;
        options.gain = static_cast<float>(gain_->value());
        options.latency_frames = latency_->currentText().toUInt();
        options.bypass = bypass_->isChecked();
        return options;
    }

    void applyOptions(const vst_host::audio::PipeWireOptions& options) {
        chainList_->clear();
        for (const auto& path : options.plugin_paths)
            chainList_->addItem(QString::fromStdString(path));
        pluginDirectories_->clear();
        for (const auto& directory : options.plugin_directories)
            pluginDirectories_->addItem(QString::fromStdString(directory));
        windowsPluginDirectories_->clear();
        for (const auto& directory : options.windows_plugin_directories)
            windowsPluginDirectories_->addItem(QString::fromStdString(directory));
        scanPluginDirectories();
        inputDevice_->setEditText(QString::fromStdString(options.input_target));
        outputName_->setText(QString::fromStdString(options.output_name));
        gain_->setValue(options.gain);
        latency_->setCurrentText(QString::number(options.latency_frames));
        bypass_->setChecked(options.bypass);
        if (chainList_->count() > 0) chainList_->setCurrentRow(0);
    }

    void scanPluginDirectories() {
        std::vector<std::string> directories;
        for (int i = 0; i < pluginDirectories_->count(); ++i)
            directories.push_back(pluginDirectories_->item(i)->text().toStdString());
        std::string warning;
        const auto discovered = vst_host::audio::discover_vst3_plugins(directories, &warning);
        for (const auto& path : discovered) {
            const auto qpath = QString::fromStdString(path);
            bool exists = false;
            for (int i = 0; i < chainList_->count(); ++i)
                if (chainList_->item(i)->text() == qpath) exists = true;
            if (!exists) chainList_->addItem(qpath);
        }
        if (!warning.empty()) status_->setText(QString::fromStdString(warning));
        if (chainList_->count() > 0 && chainList_->currentRow() < 0)
            chainList_->setCurrentRow(0);
    }

    void loadConfiguration() {
        const auto path = QFileDialog::getOpenFileName(
            this, tr("Load host configuration"), QString(),
            tr("VST Host configuration (*.vsthost);;All files (*)"));
        if (path.isEmpty()) return;
        auto options = vst_host::audio::PipeWireOptions{};
        std::string error;
        if (!vst_host::audio::load_config(path.toStdString(), options, error)) {
            QMessageBox::critical(this, tr("Configuration could not be loaded"),
                                  QString::fromStdString(error));
            return;
        }
        applyOptions(options);
        saveSettings();
    }

    void saveConfiguration() {
        const auto path = QFileDialog::getSaveFileName(
            this, tr("Save host configuration"), QStringLiteral("vst-host.vsthost"),
            tr("VST Host configuration (*.vsthost)"));
        if (path.isEmpty()) return;
        std::string error;
        if (!vst_host::audio::save_config(path.toStdString(), optionsFromUi(), error)) {
            QMessageBox::critical(this, tr("Configuration could not be saved"),
                                  QString::fromStdString(error));
            return;
        }
        status_->setText(tr("Configuration saved."));
    }

    void moveSelectedPlugin(int offset) {
        const int row = chainList_->currentRow();
        const int target = row + offset;
        if (row < 0 || target < 0 || target >= chainList_->count()) return;
        auto* item = chainList_->takeItem(row);
        chainList_->insertItem(target, item);
        chainList_->setCurrentRow(target);
    }

    void startAudio() {
        if (deviceScanPending_) {
            status_->setText(tr("Wait for microphone discovery to finish before starting audio."));
            return;
        }
        if (worker_.joinable()) worker_.join();
        saveSettings();
        if (chainList_->count() == 0 && windowsPluginDirectories_->count() == 0) {
            QMessageBox::warning(this, tr("Plugin chain is empty"),
                                 tr("Add a native Linux VST3 effect or a Windows VST3 search directory."));
            return;
        }
        if (outputName_->text().trimmed().isEmpty()) {
            QMessageBox::warning(this, tr("Name required"),
                                 tr("Enter a name for the virtual microphone."));
            return;
        }

        vst_host::audio::PipeWireOptions options;
        for (int i = 0; i < chainList_->count(); ++i)
            options.plugin_paths.push_back(chainList_->item(i)->text().toStdString());
        for (int i = 0; i < pluginDirectories_->count(); ++i)
            options.plugin_directories.push_back(pluginDirectories_->item(i)->text().toStdString());
        for (int i = 0; i < windowsPluginDirectories_->count(); ++i)
            options.windows_plugin_directories.push_back(windowsPluginDirectories_->item(i)->text().toStdString());
        options.input_target = inputTargetValue().toStdString();
        options.output_name = outputName_->text().trimmed().toStdString();
        options.output_description = options.output_name;
        options.gain = static_cast<float>(gain_->value());
        options.latency_frames = latency_->currentText().toUInt();
        options.bypass = bypass_->isChecked();

        engine_ = std::make_unique<vst_host::audio::PipeWireEngine>(std::move(options));
        engineFinished_.store(false, std::memory_order_release);
        engineResult_ = 0;
        worker_ = std::thread([this] {
            engineResult_ = engine_->run();
            engineFinished_.store(true, std::memory_order_release);
        });

        startButton_->setEnabled(false);
        stopButton_->setEnabled(true);
        chainList_->setEnabled(false);
        addPluginButton_->setEnabled(false);
        removePluginButton_->setEnabled(false);
        moveUpButton_->setEnabled(false);
        moveDownButton_->setEnabled(false);
        editorButton_->setEnabled(false);
        status_->setText(options.windows_plugin_directories.empty()
            ? tr("Starting audio engine…")
            : tr("Preparing Windows plugins through Wine/yabridge…"));
    }

    void stopAudio() {
        if (editorDialog_) {
            editorDialog_->close();
            editorDialog_ = nullptr;
        }
        if (engine_) engine_->request_stop();
        if (worker_.joinable()) worker_.join();
        engine_.reset();
        startButton_->setEnabled(true);
        stopButton_->setEnabled(false);
        chainList_->setEnabled(true);
        addPluginButton_->setEnabled(true);
        removePluginButton_->setEnabled(true);
        moveUpButton_->setEnabled(true);
        moveDownButton_->setEnabled(true);
        editorButton_->setEnabled(false);
        status_->setText(tr("Audio engine stopped."));
        saveSettings();
    }

    void openEditor() {
        if (editorDialog_) {
            editorDialog_->raise();
            editorDialog_->activateWindow();
            return;
        }
        if (QGuiApplication::platformName() != QStringLiteral("xcb")) {
            QMessageBox::information(
                this, tr("X11 editor embedding required"),
                tr("The current Qt platform is '%1'. Start this application with "
                   "QT_QPA_PLATFORM=xcb in an X11/XWayland session to embed this plugin editor.")
                    .arg(QGuiApplication::platformName()));
            return;
        }
        if (!engine_) return;
        const int selectedIndex = chainList_->currentRow();
        if (selectedIndex < 0) {
            QMessageBox::information(this, tr("Select a plugin"),
                                     tr("Select a chain entry to open its editor."));
            return;
        }
        auto* processor = engine_->plugin_processor(static_cast<std::size_t>(selectedIndex));
        if (processor == nullptr) {
            QMessageBox::warning(this, tr("Plugin not ready"),
                                 tr("Wait for the plugin to finish loading, then try again."));
            return;
        }

        auto* dialog = new PluginEditorDialog(processor, this);
        QString error;
        if (!dialog->attachEditor(error)) {
            dialog->close();
            delete dialog;
            QMessageBox::warning(this, tr("Could not open plugin editor"), error);
            return;
        }
        editorDialog_ = dialog;
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        connect(dialog, &QDialog::finished, this, [this] { editorDialog_ = nullptr; });
    }

    QString inputTargetValue() const {
        if (inputDevice_->currentIndex() >= 0 && inputDevice_->currentData().isValid())
            return inputDevice_->currentData().toString();
        return inputDevice_->currentText().trimmed();
    }

    void refreshInputDevices() {
        if (deviceScanPending_ || engine_) return;
        deviceScanPending_ = true;
        status_->setText(tr("Scanning PipeWire input devices…"));
        deviceScanFuture_ = std::async(std::launch::async, [] {
            std::string error;
            auto devices = vst_host::audio::list_pipewire_input_devices(error);
            return std::make_pair(std::move(devices), std::move(error));
        });
    }

    void pollEngine() {
        if (deviceScanPending_ && deviceScanFuture_.valid() &&
            deviceScanFuture_.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
            auto result = deviceScanFuture_.get();
            deviceScanPending_ = false;
            const auto previousTarget = inputTargetValue();
            inputDevice_->clear();
            inputDevice_->addItem(tr("Default microphone"), QString());
            for (const auto& device : result.first) {
                const auto label = QString::fromStdString(device.description) +
                    QStringLiteral(" [") + QString::number(device.id) + QStringLiteral("]");
                inputDevice_->addItem(label, QString::fromStdString(device.name));
            }
            int selected = inputDevice_->findData(previousTarget);
            if (selected >= 0) inputDevice_->setCurrentIndex(selected);
            else inputDevice_->setEditText(previousTarget);
            status_->setText(result.second.empty()
                ? tr("Found %1 PipeWire input source(s).").arg(result.first.size())
                : QString::fromStdString(result.second));
        }
        if (!engine_) return;
        if (engineFinished_.load(std::memory_order_acquire)) {
            const auto engineError = engine_->last_error();
            if (editorDialog_) {
                editorDialog_->close();
                editorDialog_ = nullptr;
            }
            if (worker_.joinable()) worker_.join();
            engine_.reset();
            startButton_->setEnabled(true);
            stopButton_->setEnabled(false);
            editorButton_->setEnabled(false);
            chainList_->setEnabled(true);
            addPluginButton_->setEnabled(true);
            removePluginButton_->setEnabled(true);
            moveUpButton_->setEnabled(true);
            moveDownButton_->setEnabled(true);
            status_->setText(!engineError.empty()
                                 ? QString::fromStdString(engineError)
                                 : (engineResult_ == 0
                                        ? tr("Audio engine stopped.")
                                        : tr("Audio engine exited with an error. See the terminal output for details.")));
            return;
        }
        const auto stats = engine_->diagnostics();
        diagnostics_->setText(tr("Buffer diagnostics — underrun: %1 frames; dropped: %2 frames")
                                  .arg(stats.underrun_frames).arg(stats.dropped_frames));
        if (engine_->plugin_processor() != nullptr) {
            while (static_cast<std::size_t>(chainList_->count()) < engine_->plugin_count()) {
                const auto path = engine_->plugin_path(static_cast<std::size_t>(chainList_->count()));
                if (path.empty()) break;
                chainList_->addItem(QString::fromStdString(path));
            }
            editorButton_->setEnabled(true);
            status_->setText(tr("Audio is running. Plugin editor is available."));
        }
    }

    QLineEdit* pluginPath_{};
    QListWidget* chainList_{};
    QListWidget* pluginDirectories_{};
    QListWidget* windowsPluginDirectories_{};
    QPushButton* addPluginButton_{};
    QPushButton* removePluginButton_{};
    QPushButton* moveUpButton_{};
    QPushButton* moveDownButton_{};
    QComboBox* inputDevice_{};
    QLineEdit* outputName_{};
    QDoubleSpinBox* gain_{};
    QComboBox* latency_{};
    QCheckBox* bypass_{};
    QPushButton* startButton_{};
    QPushButton* stopButton_{};
    QPushButton* editorButton_{};
    QLabel* status_{};
    QLabel* diagnostics_{};
    QTimer* pollTimer_{};
    std::unique_ptr<vst_host::audio::PipeWireEngine> engine_;
    std::thread worker_;
    std::future<std::pair<std::vector<vst_host::audio::PipeWireInputDevice>, std::string>> deviceScanFuture_;
    bool deviceScanPending_{false};
    std::atomic<bool> engineFinished_{false};
    int engineResult_{0};
    QPointer<PluginEditorDialog> editorDialog_;
};

} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    MainWindow window;
    window.show();
    return app.exec();
}
