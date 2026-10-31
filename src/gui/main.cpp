#include "audio/pipewire_engine.hpp"
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
#include <QMainWindow>
#include <QMessageBox>
#include <QPushButton>
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
#include <thread>
#include <algorithm>
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
        auto* browse = new QPushButton(tr("Browse…"), pluginRow);
        pluginLayout->addWidget(pluginPath_, 1);
        pluginLayout->addWidget(browse);
        auto* addPlugin = new QPushButton(tr("Add effect"), pluginRow);
        pluginLayout->addWidget(addPlugin);
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

        inputTarget_ = new QLineEdit(central);
        inputTarget_->setPlaceholderText(tr("Leave empty to use the default microphone"));
        form->addRow(tr("PipeWire input target"), inputTarget_);

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
        auto* note = new QLabel(
            tr("Plugin editor embedding currently requires the Qt X11 (xcb) platform. "
               "Wayland-native editor embedding is not implemented yet."), central);
        note->setWordWrap(true);
        root->addWidget(note);
        setCentralWidget(central);

        connect(browse, &QPushButton::clicked, this, [this] {
            const auto path = QFileDialog::getExistingDirectory(
                this, tr("Select VST3 bundle"), pluginPath_->text());
            if (!path.isEmpty()) pluginPath_->setText(path);
        });
        connect(startButton_, &QPushButton::clicked, this, [this] { startAudio(); });
        connect(stopButton_, &QPushButton::clicked, this, [this] { stopAudio(); });
        connect(editorButton_, &QPushButton::clicked, this, [this] { openEditor(); });

        pollTimer_ = new QTimer(this);
        pollTimer_->setInterval(200);
        connect(pollTimer_, &QTimer::timeout, this, [this] { pollEngine(); });
        pollTimer_->start();
    }

    ~MainWindow() override {
        stopAudio();
    }

protected:
    void closeEvent(QCloseEvent* event) override {
        stopAudio();
        QMainWindow::closeEvent(event);
    }

private:
    void moveSelectedPlugin(int offset) {
        const int row = chainList_->currentRow();
        const int target = row + offset;
        if (row < 0 || target < 0 || target >= chainList_->count()) return;
        auto* item = chainList_->takeItem(row);
        chainList_->insertItem(target, item);
        chainList_->setCurrentRow(target);
    }

    void startAudio() {
        if (worker_.joinable()) worker_.join();
        if (chainList_->count() == 0) {
            QMessageBox::warning(this, tr("Plugin chain is empty"),
                                 tr("Add at least one native Linux VST3 effect to the chain."));
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
        options.input_target = inputTarget_->text().trimmed().toStdString();
        options.output_name = outputName_->text().trimmed().toStdString();
        options.output_description = options.output_name;
        options.gain = static_cast<float>(gain_->value());
        options.latency_frames = latency_->currentText().toUInt();
        options.bypass = bypass_->isChecked();

        options.preloaded_plugins.reserve(options.plugin_paths.size());
        for (std::size_t i = 0; i < options.plugin_paths.size(); ++i) {
            auto plugin = std::make_shared<vst_host::audio::Vst3Processor>(options.plugin_paths[i]);
            std::string pluginError;
            if (!plugin->load(pluginError)) {
                QMessageBox::critical(
                    this, tr("Could not load plugin %1").arg(i + 1),
                    QString::fromStdString(pluginError));
                return;
            }
            options.preloaded_plugins.push_back(std::move(plugin));
        }

        engine_ = std::make_unique<vst_host::audio::PipeWireEngine>(std::move(options));
        engineFinished_.store(false, std::memory_order_release);
        engineResult_ = 0;
        worker_ = std::thread([this] {
            engineResult_ = engine_->run();
            engineFinished_.store(true, std::memory_order_release);
        });

        startButton_->setEnabled(false);
        stopButton_->setEnabled(true);
        editorButton_->setEnabled(false);
        status_->setText(tr("Starting audio engine…"));
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
        editorButton_->setEnabled(false);
        status_->setText(tr("Audio engine stopped."));
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
        const int selectedIndex = chainList_->currentRow();\n        if (selectedIndex < 0) {\n            QMessageBox::information(this, tr("Select a plugin"), tr("Select a chain entry to open its editor."));\n            return;\n        }\n        auto* processor = engine_->plugin_processor(static_cast<std::size_t>(selectedIndex));
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

    void pollEngine() {
        if (!engine_) return;
        if (engineFinished_.load(std::memory_order_acquire)) {
            if (editorDialog_) {
                editorDialog_->close();
                editorDialog_ = nullptr;
            }
            if (worker_.joinable()) worker_.join();
            engine_.reset();
            startButton_->setEnabled(true);
            stopButton_->setEnabled(false);
            editorButton_->setEnabled(false);
            status_->setText(engineResult_ == 0
                                 ? tr("Audio engine stopped.")
                                 : tr("Audio engine exited with an error. See the terminal output for details."));
            return;
        }
        if (engine_->plugin_processor() != nullptr) {
            editorButton_->setEnabled(true);
            status_->setText(tr("Audio is running. Plugin editor is available."));
        }
    }

    QLineEdit* pluginPath_{};\n    QListWidget* chainList_{};\n    QPushButton* removePluginButton_{};\n    QPushButton* moveUpButton_{};\n    QPushButton* moveDownButton_{};
    QLineEdit* inputTarget_{};
    QLineEdit* outputName_{};
    QDoubleSpinBox* gain_{};
    QComboBox* latency_{};
    QCheckBox* bypass_{};
    QPushButton* startButton_{};
    QPushButton* stopButton_{};
    QPushButton* editorButton_{};
    QLabel* status_{};
    QTimer* pollTimer_{};
    std::unique_ptr<vst_host::audio::PipeWireEngine> engine_;
    std::thread worker_;
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
