#include "audio/pipewire_engine.hpp"
#include "audio/config.hpp"
#include "audio/pipewire_devices.hpp"
#include "audio/plugin_discovery.hpp"
#include "audio/windows_bridge_manager.hpp"
#include "audio/windows_plugin_scope.hpp"
#include "audio/vst3_processor.hpp"

#include <QApplication>
#include <QByteArray>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QEvent>
#include <QResizeEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QSplitter>
#include <QShortcut>
#include <QKeySequence>
#include <QToolButton>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QAbstractItemView>
#include <QAbstractItemModel>
#include <QMainWindow>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QTabWidget>
#include <QFrame>
#include <QSettings>
#include <QSignalBlocker>
#include <QSet>
#include <QPointer>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#if defined(VST_HOST_HAVE_X11)
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#endif

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <future>
#include <optional>
#include <functional>
#include <thread>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <cmath>
#include <vector>
#include <utility>

namespace {

// The XEmbed editor parent can be either a child of the Qt xcb window or
// an independent X11/XWayland top-level window. The latter lets the main
// Qt GUI remain Wayland-native while older plugins use their existing X11 UI.
#if defined(VST_HOST_HAVE_X11)
class XEmbedSurface final {
public:
    explicit XEmbedSurface(Window qtParent, QWidget* focusOwner,
                           bool standalone = false,
                           std::function<void()> closeRequested = {})
        : focusOwner_(focusOwner), standalone_(standalone),
          closeRequested_(std::move(closeRequested)) {
        display_ = XOpenDisplay(nullptr);
        if (!display_) {
            error_ = "Could not open the X11 display for the VST3 editor host.";
            std::fprintf(stderr, "[VST3 XEmbed] XOpenDisplay failed (DISPLAY=%s).\n",
                         std::getenv("DISPLAY") ? std::getenv("DISPLAY") : "<unset>");
            return;
        }
        const Window nativeParent = standalone_
            ? DefaultRootWindow(display_) : qtParent;
        std::fprintf(stderr, "[VST3 XEmbed] Connected to display '%s'; parent=0x%llx (%s).\n",
                     DisplayString(display_),
                     static_cast<unsigned long long>(nativeParent),
                     standalone_ ? "separate XWayland window" : "Qt xcb child");
        XWindowAttributes parentAttributes{};
        if (!XGetWindowAttributes(display_, nativeParent, &parentAttributes)) {
            error_ = "The editor parent is not a valid X11 window on this display.";
            std::fprintf(stderr, "[VST3 XEmbed] ERROR: Qt parent window is not valid on this X11 display.\n");
            XCloseDisplay(display_);
            display_ = nullptr;
            return;
        }
        std::fprintf(stderr, "[VST3 XEmbed] Qt parent geometry=%dx%d at (%d,%d).\n",
                     parentAttributes.width, parentAttributes.height,
                     parentAttributes.x, parentAttributes.y);
        XSetWindowAttributes attributes{};
        // Use a normal X11 child: Wine editors may create focusable children.
        attributes.override_redirect = False;
        window_ = XCreateWindow(display_, nativeParent, 0, 0, 320, 180, 0, CopyFromParent,
                                InputOutput, CopyFromParent, CWOverrideRedirect, &attributes);
        if (!window_) {
            error_ = "Could not create the native X11 editor container.";
            XCloseDisplay(display_);
            display_ = nullptr;
            return;
        }
        XSelectInput(display_, window_, StructureNotifyMask | SubstructureNotifyMask |
                                         PropertyChangeMask | FocusChangeMask | ExposureMask);
        if (standalone_) {
            XStoreName(display_, window_, "VST Host - Plugin Editor");
            wmProtocolsAtom_ = XInternAtom(display_, "WM_PROTOCOLS", False);
            wmDeleteAtom_ = XInternAtom(display_, "WM_DELETE_WINDOW", False);
            XSetWMProtocols(display_, window_, &wmDeleteAtom_, 1);
        }
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
    bool standalone() const { return standalone_; }
    Window window() const { return window_; }
    const std::string& error() const { return error_; }

    void raiseWindow() {
        if (!valid() || !standalone_) return;
        XRaiseWindow(display_, window_);
        XFlush(display_);
    }

    void editorAttached() {
        editorAttached_ = true;
        attachTimer_.start();
        noChildWarningPrinted_ = false;
        std::fprintf(stderr,
                     "[VST3 XEmbed] VST3 attached() returned success; waiting for plugin child-window creation.\n");
    }

    void focusPlugin() {
        if (!valid() || children_.empty()) return;
        if (focusOwner_) focusOwner_->setFocus(Qt::OtherFocusReason);
        // XEmbed logical focus alone is insufficient for native key delivery.
        XSetInputFocus(display_, children_.front(), RevertToParent, CurrentTime);
        sendXEmbedMessage(children_.front(), 4); // XEMBED_FOCUS_IN
        XFlush(display_);
    }

    void setActive(bool active) {
        if (!valid() || children_.empty()) return;
        for (const auto child : children_) {
            sendXEmbedMessage(child, active ? 1 : 2); // WINDOW_ACTIVATE/DEACTIVATE
            sendXEmbedMessage(child, active ? 4 : 5); // FOCUS_IN/OUT
        }
        XFlush(display_);
    }

    void resizeTo(int width, int height) {
        lastWidth_ = std::max(1, width);
        lastHeight_ = std::max(1, height);
        if (!valid()) return;
        XResizeWindow(display_, window_, static_cast<unsigned int>(lastWidth_),
                      static_cast<unsigned int>(lastHeight_));
        // Never resize popup or auxiliary windows created by Wine.
        if (!children_.empty()) {
            XMoveResizeWindow(display_, children_.front(), 0, 0,
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
        message.xclient.data.l[3] = 0;
        message.xclient.data.l[4] = 0;
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
                    // Only the first direct child is the hosted editor.
                    if (!children_.empty()) break;
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
                    if (focusOwner_ && focusOwner_->hasFocus()) focusPlugin();
                    XWindowAttributes childAttributes{};
                    if (XGetWindowAttributes(display_, child, &childAttributes)) {
                        std::fprintf(stderr,
                                     "[VST3 XEmbed] Child after map: map_state=%d geometry=%dx%d.\n",
                                     childAttributes.map_state, childAttributes.width,
                                     childAttributes.height);
                    }
                }
                break;
            case ReparentNotify:
                // Bridged plug-ins often create the editor in another X11
                // process and reparent it into our socket. That does not
                // generate CreateNotify on the socket: observe it explicitly.
                if (event.xreparent.parent == window_ && children_.empty()) {
                    const Window child = event.xreparent.window;
                    children_.push_back(child);
                    sendEmbeddedNotify(child);
                    XMapWindow(display_, child);
                    XMoveResizeWindow(display_, child, 0, 0,
                                      static_cast<unsigned int>(lastWidth_),
                                      static_cast<unsigned int>(lastHeight_));
                    if (focusOwner_ && focusOwner_->hasFocus()) focusPlugin();
                    XFlush(display_);
                } else if (event.xreparent.parent != window_) {
                    children_.erase(std::remove(children_.begin(), children_.end(),
                                                event.xreparent.window), children_.end());
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
                setActive(true);
                break;
            case FocusOut:
                setActive(false);
                break;
            case ClientMessage:
                if (standalone_ && !closeRequestedSent_ &&
                    event.xclient.window == window_ &&
                    event.xclient.message_type == wmProtocolsAtom_ &&
                    static_cast<Atom>(event.xclient.data.l[0]) == wmDeleteAtom_) {
                    // A window manager requests closure. Schedule Qt dialog
                    // teardown after this event pump to avoid deleting 'this'
                    // while its member function is still executing.
                    closeRequestedSent_ = true;
                    if (closeRequested_) closeRequested_();
                } else if (event.xclient.message_type == xembedAtom_ &&
                           event.xclient.data.l[1] == 3) { // XEMBED_REQUEST_FOCUS
                    focusPlugin();
                }
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

    QWidget* focusOwner_{};
    bool standalone_{false};
    std::function<void()> closeRequested_;
    bool closeRequestedSent_{false};
    Display* display_{};
    Window window_{};
    Atom xembedAtom_{};
    Atom xembedInfoAtom_{};
    Atom wmProtocolsAtom_{};
    Atom wmDeleteAtom_{};
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
#endif

class PluginEditorDialog final : public QDialog {
public:
    explicit PluginEditorDialog(vst_host::audio::PluginProcessor* processor,
                                const QStringList& effectNames, int selectedIndex,
                                std::function<void(int)> selectEffect,
                                QWidget* parent = nullptr)
        : QDialog(parent), processor_(processor) {
        setWindowTitle(tr("Plugin Editor"));
        auto* layout = new QVBoxLayout(this);
        auto* selectorRow = new QHBoxLayout();
        selectorRow->addWidget(new QLabel(tr("Edit effect:"), this));
        selector_ = new QComboBox(this);
        selector_->addItems(effectNames);
        selector_->setCurrentIndex(selectedIndex);
        activeIndex_ = selectedIndex;
        selectorRow->addWidget(selector_, 1);
        layout->addLayout(selectorRow);
        editorNotice_ = new QLabel(this);
        editorNotice_->setWordWrap(true);
        editorNotice_->hide();
        layout->addWidget(editorNotice_);
        // The selector must never destroy/recreate its owning dialog while
        // QComboBox is finishing a popup selection: that caused a use-after-free.
        // Dispatch to the *main window* after the popup has fully closed.
        connect(selector_, QOverload<int>::of(&QComboBox::activated), this,
                [this, parent, selectEffect = std::move(selectEffect)](int index) {
                    if (!selectEffect || index == activeIndex_) return;
                    // The current index is stored independently so it is only
                    // changed after a successful editor switch.
                    const QPointer<PluginEditorDialog> alive(this);
                    QTimer::singleShot(75, parent, [alive, selectEffect, index] {
                        if (alive) selectEffect(index);
                    });
                });
        tabs_ = new QTabWidget(this);
        auto* nativePage = new QWidget(tabs_);
        auto* nativeLayout = new QVBoxLayout(nativePage);
        nativeLayout->setContentsMargins(0, 0, 0, 0);
        surface_ = new QWidget(nativePage);
#if defined(VST_HOST_HAVE_X11)
        // Never create a native X11 socket from a Wayland QPA window ID.
        if (QGuiApplication::platformName() == QStringLiteral("xcb")) {
            surface_->setAttribute(Qt::WA_NativeWindow);
            surface_->installEventFilter(this);
        }
#endif
        surface_->setFocusPolicy(Qt::StrongFocus);
        surface_->setMinimumSize(320, 180);
        nativeLayout->addWidget(surface_);
        tabs_->addTab(nativePage, tr("Plugin UI"));
        parameterScroll_ = new QScrollArea(tabs_);
        parameterScroll_->setWidgetResizable(true);
        tabs_->addTab(parameterScroll_, tr("Parameters (host controls)"));
        // Wayland-native Qt windows cannot contain an XEmbed child. Keep the
        // plugin fully usable through host parameters without forcing xcb.
#if defined(VST_HOST_HAVE_X11)
        const bool hasX11Embedding =
            QGuiApplication::platformName() == QStringLiteral("xcb");
#else
        constexpr bool hasX11Embedding = false;
#endif
        if (!hasX11Embedding) {
            tabs_->setTabEnabled(0, false);
            tabs_->setTabToolTip(0, tr("Custom XEmbed interfaces open in a "
                "separate XWayland window when an X11 display is available. "
                "Standard host controls remain in this Wayland dialog."));
            tabs_->setCurrentIndex(1);
        }
        // Synchronize host controls with edits made in the native plugin UI
        // whenever the user returns to this tab. No polling or cross-thread
        // VST3 calls are necessary while the window stays on Plugin UI.
        connect(tabs_, &QTabWidget::currentChanged, this, [this](int index) {
            if (index == 1 && attached_) rebuildParameterPanel();
        });
        layout->addWidget(tabs_);
        auto* editorIdle = new QTimer(this);
        editorIdle->setInterval(50);
        connect(editorIdle, &QTimer::timeout, this, [this] {
            if (!attached_ || !processor_) return;
#if defined(VST_HOST_HAVE_X11)
            // The standalone XWayland editor remains active even while its
            // host parameter controls are displayed in the Qt Wayland dialog.
            if (xembed_ && xembed_->standalone()) {
                processor_->idle_editor();
                return;
            }
#endif
            if (tabs_->currentIndex() == 0) processor_->idle_editor();
        });
        editorIdle->start();
        resize(560, 400);
    }

    ~PluginEditorDialog() override { detachEditor(); }

    void raisePluginEditor() {
#if defined(VST_HOST_HAVE_X11)
        if (xembed_) xembed_->raiseWindow();
#endif
    }

    void detachEditor() {
        // Discard old controls before releasing the editor controller. Their
        // valueChanged handlers must not call a destroyed controller.
        if (parameterScroll_) delete parameterScroll_->takeWidget();
        if (processor_ != nullptr && attached_) processor_->close_editor();
        attached_ = false;
#if defined(VST_HOST_HAVE_X11)
        xembed_.reset();
#endif
    }

    void rebuildParameterPanel() {
        if (!parameterScroll_ || !processor_) return;
        auto* panel = new QWidget(parameterScroll_);
        auto* controls = new QVBoxLayout(panel);
        auto* hint = new QLabel(
            tr("Host controls for VST2/VST3 parameters. These can be used when "
               "a native or Wine editor is unavailable or unresponsive. Not every "
               "plugin exposes all of its custom settings."), panel);
        hint->setWordWrap(true);
        controls->addWidget(hint);

        const auto parameters = processor_->editor_parameters();
        if (parameters.empty()) {
            auto* empty = new QLabel(
                tr("This effect did not report any standard plugin parameters."), panel);
            empty->setWordWrap(true);
            controls->addWidget(empty);
        }

        for (const auto& parameter : parameters) {
            auto* row = new QWidget(panel);
            auto* line = new QHBoxLayout(row);
            line->setContentsMargins(2, 2, 2, 2);
            QString name = QString::fromStdU16String(parameter.name);
            if (name.isEmpty()) name = tr("Parameter %1").arg(parameter.id);
            auto* label = new QLabel(name, row);
            label->setMinimumWidth(155);
            line->addWidget(label);

            auto* slider = new QSlider(Qt::Horizontal, row);
            const int steps = parameter.step_count > 0 && parameter.step_count <= 4096
                ? parameter.step_count : 1000;
            slider->setRange(0, steps);
            slider->setValue(static_cast<int>(std::lround(
                std::clamp(parameter.normalized_value, 0.0, 1.0) * steps)));
            slider->setEnabled(!parameter.read_only);
            line->addWidget(slider, 1);
            auto* valueLabel = new QLabel(
                QString::number(parameter.normalized_value, 'f', 3), row);
            valueLabel->setMinimumWidth(50);
            line->addWidget(valueLabel);
            connect(slider, &QSlider::valueChanged, row,
                    [this, id = parameter.id, steps, valueLabel](int value) {
                const double normalized = static_cast<double>(value) / steps;
                valueLabel->setText(QString::number(normalized, 'f', 3));
                if (processor_) (void)processor_->set_editor_parameter(id, normalized);
            });
            controls->addWidget(row);
        }
        controls->addStretch();
        parameterScroll_->setWidget(panel);
    }

    bool switchProcessor(vst_host::audio::PluginProcessor* processor, int index,
                         QString& error) {
        if (!processor) {
            error = tr("The requested effect is not ready.");
            selector_->setCurrentIndex(activeIndex_);
            return false;
        }
        // Keep this Qt dialog and its selector alive. Only the embedded VST3
        // view and X11 socket are replaced, avoiding the combo box UAF.
        auto* previousProcessor = processor_;
        const int previousIndex = activeIndex_;
        detachEditor();
        processor_ = processor;
        if (!attachEditor(error)) {
            // Restore the previous view if the requested editor cannot attach.
            // Otherwise the selector and audio-chain selection would disagree.
            detachEditor();
            processor_ = previousProcessor;
            QString restorationError;
            (void)attachEditor(restorationError);
            activeIndex_ = previousIndex;
            selector_->setCurrentIndex(previousIndex);
            return false;
        }
        activeIndex_ = index;
        selector_->setCurrentIndex(index);
        return true;
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
#if defined(VST_HOST_HAVE_X11)
        if (watched == surface_ && xembed_ &&
            event->type() == QEvent::MouseButtonPress)
            xembed_->focusPlugin();
#endif
        return QDialog::eventFilter(watched, event);
    }

    void changeEvent(QEvent* event) override {
        QDialog::changeEvent(event);
#if defined(VST_HOST_HAVE_X11)
        if (xembed_ && !xembed_->standalone() &&
            event->type() == QEvent::ActivationChange)
            xembed_->setActive(isActiveWindow());
#endif
    }

    void resizeEvent(QResizeEvent* event) override {
        QDialog::resizeEvent(event);
#if defined(VST_HOST_HAVE_X11)
        if (xembed_ && !xembed_->standalone() && attached_)
            xembed_->resizeTo(surface_->width(), surface_->height());
#endif
    }

public:

    bool attachEditor(QString& error) {
        if (processor_ == nullptr) {
            error = tr("The audio engine has not loaded a plugin yet.");
            return false;
        }
        show();
        editorNotice_->hide();
#if defined(VST_HOST_HAVE_X11)
        if (QGuiApplication::platformName() == QStringLiteral("xcb") &&
            processor_->has_native_editor()) {
            surface_->show();
            if (!attached_ && !windowInitialized_) {
                QApplication::processEvents();
                windowInitialized_ = true;
            }

            int width = 0;
            int height = 0;
            std::string detail;
            const auto nativeId = static_cast<Window>(surface_->winId());
            xembed_ = std::make_unique<XEmbedSurface>(nativeId, surface_);
            if (!xembed_->valid()) {
                error = QString::fromStdString(xembed_->error());
                return false;
            }
            auto* nativeParent = reinterpret_cast<void*>(
                static_cast<std::uintptr_t>(xembed_->window()));
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
            surface_->setFocus(Qt::OtherFocusReason);
            xembed_->setActive(isActiveWindow());
            xembed_->focusPlugin();
            rebuildParameterPanel();
            tabs_->setTabEnabled(0, true);
            tabs_->setCurrentIndex(0);
            return true;
        }

        // Qt Wayland can coexist with a separate X11/XWayland top-level
        // editor. The parent passed to the plugin is always an X11 window
        // created by Xlib, never a wl_surface or a Qt Wayland winId().
        if (QGuiApplication::platformName().startsWith(QStringLiteral("wayland")) &&
            processor_->has_native_editor()) {
            const char* display = std::getenv("DISPLAY");
            if (display != nullptr && *display != '\0') {
                xembed_ = std::make_unique<XEmbedSurface>(
                    0, nullptr, true, [this] {
                        QTimer::singleShot(0, this, [this] { close(); });
                    });
                if (xembed_->valid()) {
                    int width = 0;
                    int height = 0;
                    std::string detail;
                    auto* nativeParent = reinterpret_cast<void*>(
                        static_cast<std::uintptr_t>(xembed_->window()));
                    if (processor_->open_editor(nativeParent, width, height, detail)) {
                        attached_ = true;
                        xembed_->editorAttached();
                        xembed_->resizeTo(std::max(1, width), std::max(1, height));
                        xembed_->raiseWindow();
                        editorNotice_->setText(tr("Custom plugin editor opened in a "
                            "separate XWayland window. Closing this dialog also "
                            "closes the plugin editor."));
                        editorNotice_->show();
                    } else {
                        editorNotice_->setText(tr("Could not open the XWayland "
                            "plugin editor: %1. Standard host controls remain "
                            "available.").arg(QString::fromStdString(detail)));
                        xembed_.reset();
                    }
                } else {
                    editorNotice_->setText(
                        tr("Could not connect to the X11/XWayland display: %1. "
                           "Standard host controls remain available.")
                            .arg(QString::fromStdString(xembed_->error())));
                    xembed_.reset();
                }
            } else {
                editorNotice_->setText(
                    tr("No XWayland display is available (DISPLAY is unset). "
                       "Custom X11 plugin editors need XWayland; standard host "
                       "controls are still available."));
            }
        }
#else
        if (QGuiApplication::platformName().startsWith(QStringLiteral("wayland")) &&
            processor_->has_native_editor())
            editorNotice_->setText(
                tr("This build does not include X11 editor support. "
                   "Install X11 development headers and rebuild to use "
                   "XWayland plugin windows."));
#endif
        // Native Qt Wayland host controls remain usable even if the
        // external XEmbed window cannot open or the plugin has no custom UI.
        tabs_->setTabEnabled(0, false);
        tabs_->setCurrentIndex(1);
        rebuildParameterPanel();
        if (!editorNotice_->text().isEmpty()) editorNotice_->show();
        return true;
    }

private:
    vst_host::audio::PluginProcessor* processor_{};
    QComboBox* selector_{};
    QLabel* editorNotice_{};
    int activeIndex_{-1};
    bool windowInitialized_{false};
    QWidget* surface_{};
    QTabWidget* tabs_{};
    QScrollArea* parameterScroll_{};
#if defined(VST_HOST_HAVE_X11)
    std::unique_ptr<XEmbedSurface> xembed_;
#endif
    bool attached_{false};
};

struct PluginScanResult {
    std::vector<std::pair<std::string, bool>> plugins;
    std::size_t duplicate_aliases{};
    std::size_t unselected_window_wrappers{};
    std::string warning;
    std::optional<vst_host::audio::WindowsBridgeSelection> bridge;
};

class MainWindow final : public QMainWindow {
public:
    MainWindow() {
        setWindowTitle(tr("VST Host Experiment"));
        resize(1080, 740);
        setMinimumSize(800, 560);

        auto* central = new QWidget(this);
        auto* root = new QVBoxLayout(central);
        root->setContentsMargins(12, 12, 12, 12);
        root->setSpacing(8);

        // Two independently resizable columns avoid a tall single stack of
        // library and chain controls on typical desktop screens.
        auto* browserPanel = new QGroupBox(tr("Plugin library"), central);
        auto* browserLayout = new QVBoxLayout(browserPanel);
        browserLayout->setSpacing(6);
        auto* chainPanel = new QGroupBox(tr("Processing chain"), central);
        auto* chainLayout = new QVBoxLayout(chainPanel);
        chainLayout->setSpacing(6);
        auto* columns = new QSplitter(Qt::Horizontal, central);
        workspaceSplitter_ = columns;
        columns->setChildrenCollapsible(false);
        columns->addWidget(browserPanel);
        columns->addWidget(chainPanel);
        columns->setStretchFactor(0, 1);
        columns->setStretchFactor(1, 1);
        root->addWidget(columns, 1);

        auto* form = new QFormLayout();

        auto* pluginRow = new QWidget(central);
        auto* pluginLayout = new QHBoxLayout(pluginRow);
        pluginLayout->setContentsMargins(0, 0, 0, 0);
        pluginPath_ = new QLineEdit(pluginRow);
        pluginPath_->setPlaceholderText(tr("Choose an individual Linux .vst3 bundle"));
        pluginLayout->addWidget(pluginPath_, 1);
        addPluginButton_ = new QPushButton(tr("Add effect"), pluginRow);
        pluginLayout->addWidget(addPluginButton_);
#if defined(VST_HOST_ENABLE_VST2)
        auto* addVst2Button = new QPushButton(tr("Add VST2 .so…"), pluginRow);
        pluginLayout->addWidget(addVst2Button);
        connect(addVst2Button, &QPushButton::clicked, this, [this] {
            const auto path = QFileDialog::getOpenFileName(
                this, tr("Add Linux VST2 plugin"), QString(),
                tr("VST2 libraries (*.so);;All files (*)"));
            if (!path.isEmpty() && path.endsWith(QStringLiteral(".so"), Qt::CaseInsensitive)) {
                if (!manualPlugins_.contains(path)) manualPlugins_.append(path);
                addAvailablePlugin(path, appearsToBeWineWrapper(path));
                availablePlugins_->sortItems(Qt::AscendingOrder);
                applyPluginFilter();
                saveSettings();
            }
        });
#endif
        // The manual bundle picker belongs to the library, not audio routing.

        availablePlugins_ = new QListWidget(central);
        availablePlugins_->setMinimumHeight(130);
        availablePlugins_->setSelectionMode(QAbstractItemView::SingleSelection);
        auto* browserHeader = new QHBoxLayout();
        browserHeader->addWidget(new QLabel(tr("Available plugins"), central));
        browserHeader->addStretch(1);
        availableCount_ = new QLabel(tr("0 available"), central);
        browserHeader->addWidget(availableCount_);
        browserLayout->addLayout(browserHeader);
        pluginSearch_ = new QLineEdit(central);
        pluginSearch_->setClearButtonEnabled(true);
        pluginSearch_->setPlaceholderText(tr("Find an effect… (Ctrl+F)"));
        browserLayout->addWidget(pluginSearch_);
        connect(pluginSearch_, &QLineEdit::textChanged, this,
                [this](const QString&) { applyPluginFilter(); });
        auto* findShortcut = new QShortcut(QKeySequence::Find, this);
        connect(findShortcut, &QShortcut::activated, pluginSearch_,
                [this] { pluginSearch_->setFocus(); pluginSearch_->selectAll(); });
        browserLayout->addWidget(availablePlugins_, 1);
        auto* availableActions = new QHBoxLayout();
        addSelectedPluginButton_ = new QPushButton(tr("Add selected to chain →"), central);
        refreshPluginListButton_ = new QPushButton(tr("Refresh list"), central);
        availableActions->addWidget(addSelectedPluginButton_);
        availableActions->addWidget(refreshPluginListButton_);
        browserLayout->addLayout(availableActions);
        browserLayout->addWidget(pluginRow);
        connect(addSelectedPluginButton_, &QPushButton::clicked, this, [this] { addAvailablePluginToChain(); });
        connect(availablePlugins_, &QListWidget::itemDoubleClicked, this,
                [this](QListWidgetItem*) {
                    if (addSelectedPluginButton_->isEnabled())
                        addAvailablePluginToChain();
                });
        connect(refreshPluginListButton_, &QPushButton::clicked, this, [this] { scanPluginDirectories(); });

        chainList_ = new QListWidget(central);
        chainList_->setMinimumHeight(130);
        chainList_->setSelectionMode(QAbstractItemView::SingleSelection);
        chainList_->setDragDropMode(QAbstractItemView::InternalMove);
        chainList_->setToolTip(tr("Select an active effect; double-click it to open its editor while audio runs."));
        chainLayout->addWidget(new QLabel(tr("Effects run from top to bottom"), chainPanel));
        chainLayout->addWidget(chainList_, 1);
        selectedPluginLabel_ = new QLabel(tr("No effect selected"), central);
        selectedPluginLabel_->setWordWrap(true);
        chainLayout->addWidget(selectedPluginLabel_);
        bypassSelectedEffect_ = new QCheckBox(
            tr("Bypass selected effect (can change while audio runs)"), central);
        bypassSelectedEffect_->setEnabled(false);
        chainLayout->addWidget(bypassSelectedEffect_);
        clearSavedStateButton_ = new QPushButton(
            tr("Clear saved settings for selected effect"), central);
        clearSavedStateButton_->setEnabled(false);
        chainLayout->addWidget(clearSavedStateButton_);
        connect(clearSavedStateButton_, &QPushButton::clicked, this, [this] {
            if (engine_ || chainList_->currentRow() < 0) return;
            auto* item = chainList_->currentItem();
            item->setData(Qt::UserRole + 3, QByteArray());
            item->setData(Qt::UserRole + 4, QByteArray());
            updateSelectedPlugin();
            saveSettings();
            status_->setText(tr("Saved plugin settings cleared. The effect will start with its defaults."));
        });
        connect(bypassSelectedEffect_, &QCheckBox::toggled, this,
                [this](bool bypassed) {
                    const int row = chainList_->currentRow();
                    if (row < 0) return;
                    chainList_->item(row)->setData(Qt::UserRole + 2, bypassed);
                    updateChainLabels();
                    if (engine_)
                        (void)engine_->set_plugin_bypassed(
                            static_cast<std::size_t>(row), bypassed);
                    saveSettings();
                });
        auto* chainActions = new QHBoxLayout();
        removePluginButton_ = new QPushButton(tr("Remove selected"), central);
        moveUpButton_ = new QPushButton(tr("Move up"), central);
        moveDownButton_ = new QPushButton(tr("Move down"), central);
        chainActions->addWidget(removePluginButton_);
        chainActions->addWidget(moveUpButton_);
        chainActions->addWidget(moveDownButton_);
        chainLayout->addLayout(chainActions);

        auto* configActions = new QHBoxLayout();
        loadConfigButton_ = new QPushButton(tr("Load configuration…"), central);
        saveConfigButton_ = new QPushButton(tr("Save configuration…"), central);
        saveConfigButton_->setToolTip(tr(
            "Stop audio to capture the latest VST3 plugin state before saving a portable configuration."));
        configActions->addWidget(loadConfigButton_);
        configActions->addWidget(saveConfigButton_);
        root->addLayout(configActions);
        connect(loadConfigButton_, &QPushButton::clicked, this, [this] { loadConfiguration(); });
        connect(saveConfigButton_, &QPushButton::clicked, this, [this] { saveConfiguration(); });

        // Plugin-directory management is infrequent: collapse it by default
        // so the browser and chain get most of the vertical space.
        auto* directoryToggle = new QToolButton(browserPanel);
        directoryToggle->setText(tr("Plugin folders and scanning"));
        directoryToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        directoryToggle->setArrowType(Qt::RightArrow);
        directoryToggle->setCheckable(true);
        directoryToggle->setToolTip(tr("Expand to add or remove Linux/Wine plugin locations"));
        browserLayout->addWidget(directoryToggle);
        auto* directoryPanel = new QWidget(browserPanel);
        auto* directoryLayout = new QVBoxLayout(directoryPanel);
        directoryLayout->setContentsMargins(0, 0, 0, 0);
        pluginDirectories_ = new QListWidget(directoryPanel);
        pluginDirectories_->setMaximumHeight(110);
        directoryLayout->addWidget(pluginDirectories_);
        browserLayout->addWidget(directoryPanel);
        directoryPanel->hide();
        connect(directoryToggle, &QToolButton::toggled, this,
                [directoryToggle, directoryPanel](bool expanded) {
                    directoryPanel->setVisible(expanded);
                    directoryToggle->setArrowType(
                        expanded ? Qt::DownArrow : Qt::RightArrow);
                });
        auto* directoryActions = new QHBoxLayout();
        addDirectoryButton_ = new QPushButton(tr("Add Linux directory…"), central);
        addWindowsDirectoryButton_ = new QPushButton(tr("Add Windows directory…"), central);
        removeDirectoryButton_ = new QPushButton(tr("Remove directory"), central);
        scanDirectoriesButton_ = new QPushButton(tr("Scan all"), central);
        directoryActions->addWidget(addDirectoryButton_);
        directoryActions->addWidget(addWindowsDirectoryButton_);
        directoryActions->addWidget(removeDirectoryButton_);
        directoryActions->addWidget(scanDirectoriesButton_);
        directoryLayout->addLayout(directoryActions);
        connect(addDirectoryButton_, &QPushButton::clicked, this, [this] {
            const auto path = QFileDialog::getExistingDirectory(this, tr("Add Linux VST3 directory"));
            if (!path.isEmpty()) {
                addPluginDirectory(path, false);
                saveSettings();
                scanPluginDirectories();
            }
        });
        connect(addWindowsDirectoryButton_, &QPushButton::clicked, this, [this] {
            const auto path = QFileDialog::getExistingDirectory(this, tr("Add Windows VST3 directory"));
            if (!path.isEmpty()) {
                addPluginDirectory(path, true);
                saveSettings();
                scanPluginDirectories();
            }
        });
        connect(removeDirectoryButton_, &QPushButton::clicked, this, [this] {
            const int row = pluginDirectories_->currentRow();
            if (row >= 0) delete pluginDirectories_->takeItem(row);
            saveSettings();
            scanPluginDirectories();
        });
        connect(scanDirectoriesButton_, &QPushButton::clicked, this, [this] {
            scanPluginDirectories();
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
        inputDevice_->setToolTip(tr(
            "Choose a microphone by its stable PipeWire node name. "
            "When this device disconnects, the virtual microphone stays silent "
            "until that SAME device returns. The Default microphone option "
            "continues to follow system-wide default changes."));

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
        form->addRow(bypass_);
        auto* audioPanel = new QGroupBox(tr("Audio routing"), central);
        audioPanel->setLayout(form);
        root->insertWidget(0, audioPanel);

        auto* actions = new QHBoxLayout();
        startButton_ = new QPushButton(tr("Start audio"), central);
        stopButton_ = new QPushButton(tr("Stop audio"), central);
        editorButton_ = new QPushButton(tr("Edit selected effect"), central);
        stopButton_->setEnabled(false);
        editorButton_->setEnabled(false);
        actions->addWidget(startButton_);
        actions->addWidget(stopButton_);
        actions->addWidget(editorButton_);
        root->insertLayout(1, actions);

        status_ = new QLabel(tr("Ready. Add effects to the chain or start without effects for direct microphone routing."), central);
        status_->setWordWrap(true);
        root->addWidget(status_);
        diagnostics_ = new QLabel(tr("Audio diagnostics: not running"), central);
        root->addWidget(diagnostics_);
        auto* note = new QLabel(
            tr("Plugin-specific editors use X11/XWayland. On native Wayland, "
               "the host's standard parameter controls remain available."), central);
        note->setWordWrap(true);
        root->addWidget(note);
        auto* scrollArea = new QScrollArea(this);
        scrollArea->setWidgetResizable(true);
        scrollArea->setFrameShape(QFrame::NoFrame);
        scrollArea->setWidget(central);
        setCentralWidget(scrollArea);

        connect(addPluginButton_, &QPushButton::clicked, this, [this] {
            const auto path = QFileDialog::getExistingDirectory(
                this, tr("Add individual VST3 plugin bundle"), pluginPath_->text());
            if (!path.isEmpty()) {
                if (!path.endsWith(QStringLiteral(".vst3"), Qt::CaseInsensitive)) {
                    QMessageBox::warning(this, tr("Not a VST3 bundle"),
                        tr("Select a folder ending in .vst3, not its parent directory."));
                    return;
                }
                pluginPath_->setText(path);
                if (!manualPlugins_.contains(path)) manualPlugins_.append(path);
                addAvailablePlugin(path, false);
                availablePlugins_->sortItems(Qt::AscendingOrder);
                applyPluginFilter();
                saveSettings();
            }
        });
        connect(removePluginButton_, &QPushButton::clicked, this, [this] {
            const int row = chainList_->currentRow();
            if (row >= 0) delete chainList_->takeItem(row);
            updateChainLabels();
            saveSettings();
        });
        connect(moveUpButton_, &QPushButton::clicked, this, [this] {
            moveSelectedPlugin(-1);
            saveSettings();
        });
        connect(moveDownButton_, &QPushButton::clicked, this, [this] {
            moveSelectedPlugin(1);
            saveSettings();
        });
        connect(startButton_, &QPushButton::clicked, this, [this] { startAudio(); });
        connect(stopButton_, &QPushButton::clicked, this, [this] { stopAudio(); });
        connect(editorButton_, &QPushButton::clicked, this, [this] { openEditor(); });
        connect(chainList_, &QListWidget::currentRowChanged, this,
                [this](int) { updateSelectedPlugin(); });
        connect(chainList_, &QListWidget::itemDoubleClicked, this,
                [this](QListWidgetItem*) { if (editorButton_->isEnabled()) openEditor(); });
        connect(chainList_->model(), &QAbstractItemModel::rowsMoved, this,
                [this] {
                    updateChainLabels();
                    saveSettings();
                });

        pollTimer_ = new QTimer(this);
        pollTimer_->setInterval(200);
        connect(pollTimer_, &QTimer::timeout, this, [this] { pollEngine(); });
        loadSettings();
        // On a first launch, make plugin discovery obvious; returning users
        // with configured roots keep the workspace compact by default.
        if (pluginDirectories_->count() == 0 && manualPlugins_.isEmpty())
            directoryToggle->setChecked(true);
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
    static vst_host::audio::PluginState storedPluginState(const QListWidgetItem* item) {
        vst_host::audio::PluginState result;
        if (!item) return result;
        const auto component = item->data(Qt::UserRole + 3).toByteArray();
        const auto controller = item->data(Qt::UserRole + 4).toByteArray();
        if (static_cast<std::size_t>(component.size()) > vst_host::audio::max_plugin_state_bytes ||
            static_cast<std::size_t>(controller.size()) > vst_host::audio::max_plugin_state_bytes)
            return result;
        result.component.resize(static_cast<std::size_t>(component.size()));
        result.controller.resize(static_cast<std::size_t>(controller.size()));
        if (!component.isEmpty())
            std::memcpy(result.component.data(), component.constData(),
                        static_cast<std::size_t>(component.size()));
        if (!controller.isEmpty())
            std::memcpy(result.controller.data(), controller.constData(),
                        static_cast<std::size_t>(controller.size()));
        return result;
    }

    static void storePluginState(QListWidgetItem* item,
                                 const vst_host::audio::PluginState& state) {
        if (!item) return;
        if (state.component.size() > vst_host::audio::max_plugin_state_bytes ||
            state.controller.size() > vst_host::audio::max_plugin_state_bytes) return;
        item->setData(Qt::UserRole + 3,
            QByteArray(reinterpret_cast<const char*>(state.component.data()),
                       static_cast<qsizetype>(state.component.size())));
        item->setData(Qt::UserRole + 4,
            QByteArray(reinterpret_cast<const char*>(state.controller.data()),
                       static_cast<qsizetype>(state.controller.size())));
    }

    QString captureFinishedPluginStates() {
        if (!engine_) return {};
        const auto snapshots = engine_->saved_plugin_states();
        for (std::size_t i = 0; i < snapshots.size() &&
             i < static_cast<std::size_t>(chainList_->count()); ++i) {
            // A plugin that refuses getState should not discard a valid
            // previously loaded snapshot; keep it for troubleshooting.
            if (!snapshots[i].empty())
                storePluginState(chainList_->item(static_cast<int>(i)), snapshots[i]);
        }
        return QString::fromStdString(engine_->plugin_state_warning()).trimmed();
    }

    static QString displayPluginName(const QString& path, bool wine) {
        const QFileInfo info(path);
        QString name = info.completeBaseName();
        if (name.isEmpty()) name = info.fileName();
        if (name.isEmpty()) name = path;
        if (path.endsWith(QStringLiteral(".so"), Qt::CaseInsensitive))
            name += tr(" (VST2)");
        else if (path.endsWith(QStringLiteral(".vst3"), Qt::CaseInsensitive))
            name += tr(" (VST3)");
        if (wine) name += tr(" (WINE)");
        return name;
    }

    void applyPluginFilter() {
        if (!pluginSearch_ || !availableCount_ || !availablePlugins_) return;
        const auto query = pluginSearch_->text().trimmed();
        int visible = 0;
        for (int i = 0; i < availablePlugins_->count(); ++i) {
            auto* item = availablePlugins_->item(i);
            const bool matches = query.isEmpty() ||
                item->text().contains(query, Qt::CaseInsensitive) ||
                item->data(Qt::UserRole).toString().contains(query, Qt::CaseInsensitive);
            item->setHidden(!matches);
            if (matches) ++visible;
        }
        availableCount_->setText(tr("%1 of %2 available")
            .arg(visible).arg(availablePlugins_->count()));
    }

    void updateChainLabels() {
        for (int i = 0; i < chainList_->count(); ++i) {
            auto* item = chainList_->item(i);
            QString text = tr("%1. %2").arg(i + 1)
                .arg(displayPluginName(item->data(Qt::UserRole).toString(),
                                        item->data(Qt::UserRole + 1).toBool()));
            if (item->data(Qt::UserRole + 2).toBool())
                text += tr(" [BYPASSED]");
            item->setText(text);
        }
    }

    static bool appearsToBeWineWrapper(const QString& path) {
        return path.contains(QStringLiteral("/yabridge/")) ||
               path.contains(QStringLiteral("/vstbridge/"));
    }

    void addPluginDirectory(const QString& path, bool wine) {
        for (int i = 0; i < pluginDirectories_->count(); ++i) {
            const auto* existing = pluginDirectories_->item(i);
            if (existing->data(Qt::UserRole).toString() == path &&
                existing->data(Qt::UserRole + 1).toBool() == wine) return;
        }
        auto* item = new QListWidgetItem(
            path + (wine ? tr(" (WINE)") : QString()), pluginDirectories_);
        item->setData(Qt::UserRole, path);
        item->setData(Qt::UserRole + 1, wine);
        item->setToolTip(path);
    }

    void addChainPlugin(const QString& path, bool wine, bool bypassed = false) {
        wine = wine || appearsToBeWineWrapper(path);
        auto* item = new QListWidgetItem(displayPluginName(path, wine), chainList_);
        item->setData(Qt::UserRole, path);
        item->setData(Qt::UserRole + 1, wine);
        item->setData(Qt::UserRole + 2, bypassed);
        item->setData(Qt::UserRole + 3, QByteArray());
        item->setData(Qt::UserRole + 4, QByteArray());
        item->setToolTip(path);
        updateChainLabels();
    }

    void setEditingEnabled(bool enabled) {
        // Freeze reordering while the engine runs but keep rows selectable:
        // selecting the next plugin must not require restarting the audio.
        chainList_->setEnabled(enabled || engine_ != nullptr);
        chainList_->setDragDropMode(enabled
            ? QAbstractItemView::InternalMove : QAbstractItemView::NoDragDrop);
        availablePlugins_->setEnabled(enabled);
        addPluginButton_->setEnabled(enabled);
        addSelectedPluginButton_->setEnabled(enabled);
        refreshPluginListButton_->setEnabled(enabled);
        removePluginButton_->setEnabled(enabled);
        moveUpButton_->setEnabled(enabled);
        moveDownButton_->setEnabled(enabled);
        pluginDirectories_->setEnabled(enabled);
        addDirectoryButton_->setEnabled(enabled);
        addWindowsDirectoryButton_->setEnabled(enabled);
        removeDirectoryButton_->setEnabled(enabled);
        scanDirectoriesButton_->setEnabled(enabled);
        loadConfigButton_->setEnabled(enabled);
        saveConfigButton_->setEnabled(enabled);
        clearSavedStateButton_->setEnabled(enabled && chainList_->currentRow() >= 0);
        // Routing/gain/latency are startup-only controls. Keep the selected
        // node name fixed for the running session; only live per-effect bypass
        // can change without a restart.
        inputDevice_->setEnabled(enabled);
        outputName_->setEnabled(enabled);
        gain_->setEnabled(enabled);
        latency_->setEnabled(enabled);
        bypass_->setEnabled(enabled);
        updateSelectedPlugin();
    }

    void loadSettings() {
        QSettings settings(QStringLiteral("VSTHostExperiment"),
                           QStringLiteral("VSTHostExperiment"));
        const auto workspaceState =
            settings.value(QStringLiteral("ui/workspaceSplitter")).toByteArray();
        if (!workspaceState.isEmpty())
            (void)workspaceSplitter_->restoreState(workspaceState);
        settings.beginGroup(QStringLiteral("audio"));
        const auto paths = settings.value(QStringLiteral("pluginChain")).toStringList();
        const auto kinds = settings.value(QStringLiteral("pluginChainWine")).toList();
        const auto bypasses = settings.value(QStringLiteral("pluginChainBypassed")).toList();
        const auto componentStates = settings.value(QStringLiteral("pluginComponentStates")).toList();
        const auto controllerStates = settings.value(QStringLiteral("pluginControllerStates")).toList();
        for (int i = 0; i < paths.size(); ++i) {
            addChainPlugin(paths[i], i < kinds.size()
                ? kinds[i].toBool() : appearsToBeWineWrapper(paths[i]),
                i < bypasses.size() && bypasses[i].toBool());
            auto* item = chainList_->item(i);
            if (i < componentStates.size())
                item->setData(Qt::UserRole + 3, componentStates[i].toByteArray());
            if (i < controllerStates.size())
                item->setData(Qt::UserRole + 4, controllerStates[i].toByteArray());
        }
        manualPlugins_ = settings.value(QStringLiteral("individualPluginBundles")).toStringList();
        const auto directories = settings.value(QStringLiteral("pluginDirectories")).toStringList();
        for (const auto& directory : directories) addPluginDirectory(directory, false);
        const auto windowsDirectories = settings.value(QStringLiteral("windowsPluginDirectories")).toStringList();
        for (const auto& directory : windowsDirectories) addPluginDirectory(directory, true);
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
        QVariantList kinds;
        QVariantList bypasses;
        QVariantList componentStates;
        QVariantList controllerStates;
        for (int i = 0; i < chainList_->count(); ++i) {
            const auto* item = chainList_->item(i);
            paths.push_back(item->data(Qt::UserRole).toString());
            kinds.push_back(item->data(Qt::UserRole + 1).toBool());
            bypasses.push_back(item->data(Qt::UserRole + 2).toBool());
            componentStates.push_back(item->data(Qt::UserRole + 3).toByteArray());
            controllerStates.push_back(item->data(Qt::UserRole + 4).toByteArray());
        }
        settings.setValue(QStringLiteral("pluginChain"), paths);
        settings.setValue(QStringLiteral("pluginChainWine"), kinds);
        settings.setValue(QStringLiteral("pluginChainBypassed"), bypasses);
        settings.setValue(QStringLiteral("pluginComponentStates"), componentStates);
        settings.setValue(QStringLiteral("pluginControllerStates"), controllerStates);
        settings.setValue(QStringLiteral("individualPluginBundles"), manualPlugins_);
        QStringList directories;
        QStringList windowsDirectories;
        for (int i = 0; i < pluginDirectories_->count(); ++i) {
            const auto* item = pluginDirectories_->item(i);
            (item->data(Qt::UserRole + 1).toBool() ? windowsDirectories : directories)
                .push_back(item->data(Qt::UserRole).toString());
        }
        settings.setValue(QStringLiteral("pluginDirectories"), directories);
        settings.setValue(QStringLiteral("windowsPluginDirectories"), windowsDirectories);
        settings.setValue(QStringLiteral("inputTarget"), inputTargetValue());
        settings.setValue(QStringLiteral("outputName"), outputName_->text());
        settings.setValue(QStringLiteral("gain"), gain_->value());
        settings.setValue(QStringLiteral("latencyFrames"), latency_->currentText());
        settings.setValue(QStringLiteral("gainBypassed"), bypass_->isChecked());
        settings.endGroup();
        settings.setValue(QStringLiteral("ui/workspaceSplitter"),
                          workspaceSplitter_->saveState());
        settings.sync();
    }

    vst_host::audio::PipeWireOptions optionsFromUi() const {
        vst_host::audio::PipeWireOptions options;
        for (int i = 0; i < chainList_->count(); ++i) {
            const auto* item = chainList_->item(i);
            options.plugin_paths.push_back(item->data(Qt::UserRole).toString().toStdString());
            options.plugin_bypassed.push_back(item->data(Qt::UserRole + 2).toBool());
            options.plugin_states.push_back(storedPluginState(item));
        }
        for (int i = 0; i < pluginDirectories_->count(); ++i) {
            const auto* item = pluginDirectories_->item(i);
            auto& directories = item->data(Qt::UserRole + 1).toBool()
                ? options.windows_plugin_directories : options.plugin_directories;
            directories.push_back(item->data(Qt::UserRole).toString().toStdString());
        }
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
        for (std::size_t index = 0; index < options.plugin_paths.size(); ++index) {
            const auto qpath = QString::fromStdString(options.plugin_paths[index]);
            addChainPlugin(qpath, appearsToBeWineWrapper(qpath),
                index < options.plugin_bypassed.size() && options.plugin_bypassed[index]);
            if (index < options.plugin_states.size())
                storePluginState(chainList_->item(static_cast<int>(index)),
                                 options.plugin_states[index]);
        }
        pluginDirectories_->clear();
        for (const auto& directory : options.plugin_directories)
            addPluginDirectory(QString::fromStdString(directory), false);
        for (const auto& directory : options.windows_plugin_directories)
            addPluginDirectory(QString::fromStdString(directory), true);
        scanPluginDirectories();
        inputDevice_->setEditText(QString::fromStdString(options.input_target));
        outputName_->setText(QString::fromStdString(options.output_name));
        gain_->setValue(options.gain);
        latency_->setCurrentText(QString::number(options.latency_frames));
        bypass_->setChecked(options.bypass);
        if (chainList_->count() > 0) chainList_->setCurrentRow(0);
    }

    void addAvailablePlugin(const QString& path, bool wine) {
        if (path.trimmed().isEmpty() || availablePluginPaths_.contains(path)) return;
        wine = wine || appearsToBeWineWrapper(path);
        availablePluginPaths_.insert(path);
        auto* item = new QListWidgetItem(displayPluginName(path, wine), availablePlugins_);
        item->setData(Qt::UserRole, path);
        item->setData(Qt::UserRole + 1, wine);
        item->setToolTip(path);
    }

    void addAvailablePluginToChain() {
        if (engine_ || pluginScanPending_) return;
        auto* selected = availablePlugins_->currentItem();
        if (!selected || selected->isHidden()) return;
        // Repeated instances of the same VST are valid and can use different settings.
        addChainPlugin(selected->data(Qt::UserRole).toString(),
                       selected->data(Qt::UserRole + 1).toBool());
        chainList_->setCurrentRow(chainList_->count() - 1);
        saveSettings();
    }

    void scanPluginDirectories() {
        if (engine_ || pluginScanPending_) return;
        std::vector<std::string> nativeDirectories;
        std::vector<std::string> wineDirectories;
        for (int i = 0; i < pluginDirectories_->count(); ++i) {
            const auto* item = pluginDirectories_->item(i);
            (item->data(Qt::UserRole + 1).toBool() ? wineDirectories : nativeDirectories)
                .push_back(item->data(Qt::UserRole).toString().toStdString());
        }
        std::vector<std::pair<std::string, bool>> explicitPlugins;
        for (const auto& path : manualPlugins_)
            explicitPlugins.emplace_back(path.toStdString(), false);
        for (int i = 0; i < chainList_->count(); ++i) {
            const auto* item = chainList_->item(i);
            explicitPlugins.emplace_back(item->data(Qt::UserRole).toString().toStdString(),
                                         item->data(Qt::UserRole + 1).toBool());
        }

        pluginScanPending_ = true;
        setEditingEnabled(false);
        status_->setText(tr("Scanning Linux and Windows plugins…"));
        pluginScanFuture_ = std::async(
            std::launch::async,
            [nativeDirectories = std::move(nativeDirectories),
             wineDirectories = std::move(wineDirectories),
             explicitPlugins = std::move(explicitPlugins)]() mutable {
                PluginScanResult result;
                result.plugins = std::move(explicitPlugins);
                auto native = vst_host::audio::discover_vst3_plugins(
                    nativeDirectories, &result.warning);
                for (auto& path : native) result.plugins.emplace_back(std::move(path), false);
#if defined(VST_HOST_ENABLE_VST2)
                auto nativeVst2 = vst_host::audio::discover_vst2_plugins(
                    nativeDirectories, &result.warning);
                for (auto& path : nativeVst2) result.plugins.emplace_back(std::move(path), false);
#endif
                if (!wineDirectories.empty()) {
                    vst_host::audio::PipeWireOptions bridgeOptions;
                    bridgeOptions.windows_plugin_directories = std::move(wineDirectories);
                    const auto selectedWindowsRoots = bridgeOptions.windows_plugin_directories;
                    std::string error;
                    vst_host::audio::WindowsBridgeSelection selectedBridge;
                    if (!vst_host::audio::prepare_windows_vst3_plugins(
                            bridgeOptions, error, &selectedBridge, &result.warning)) {
                        result.warning += error + "\n";
                    } else {
                        result.bridge = selectedBridge;
                        // Restore existing chains/configurations using the
                        // equivalent vstbridge wrapper only after it exists.
                        // Never accidentally reload yabridge for a migrated entry.
                        for (auto& entry : result.plugins) {
                            if (entry.second)
                                entry.first = vst_host::audio::plugin_format(entry.first) ==
                                    vst_host::audio::PluginFormat::Vst2
                                    ? vst_host::audio::migrate_vst2_wrapper_path(
                                        entry.first, selectedBridge)
                                    : vst_host::audio::detail::migrate_vst3_wrapper_path(
                                        entry.first, selectedBridge);
                        }
                        // Bridge wrappers are discovered, but never appended
                        // to the running chain without an explicit selection.
                        auto wrappers = vst_host::audio::discover_vst3_plugins(
                            bridgeOptions.plugin_directories, &result.warning);
                        std::size_t omitted = 0;
                        wrappers = vst_host::audio::filter_bridge_plugins_to_selected_roots(
                            wrappers, selectedWindowsRoots, &omitted);
                        result.unselected_window_wrappers += omitted;
                        for (auto& path : wrappers)
                            result.plugins.emplace_back(std::move(path), true);
#if defined(VST_HOST_ENABLE_VST2)
                        auto vst2Wrappers = vst_host::audio::discover_vst2_plugins(
                            bridgeOptions.plugin_directories, &result.warning);
                        vst2Wrappers = vst_host::audio::filter_bridge_plugins_to_selected_roots(
                            vst2Wrappers, selectedWindowsRoots, &omitted);
                        result.unselected_window_wrappers += omitted;
                        for (auto& path : vst2Wrappers)
                            result.plugins.emplace_back(std::move(path), true);
#endif
                    }
                }
                // Both vstbridge and yabridge can mirror the same Windows
                // module under multiple relative paths when overlapping
                // directories were registered. The GUI's previous path-only
                // QSet check did not detect these aliases.
                result.plugins = vst_host::audio::deduplicate_vst3_browser_entries(
                    result.plugins, &result.duplicate_aliases);
                return result;
            });
    }

    void loadConfiguration() {
        if (engine_ || pluginScanPending_) return;
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
        if (engine_) {
            QMessageBox::information(this, tr("Stop audio before saving"),
                tr("Stop the audio engine first so the host can safely capture "
                   "the current state of every effect."));
            return;
        }
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
        updateChainLabels();
        updateSelectedPlugin();
    }

    void startAudio() {
        if (deviceScanPending_ || pluginScanPending_) {
            status_->setText(tr("Finish microphone and plugin discovery before starting audio."));
            return;
        }
        if (worker_.joinable()) worker_.join();
        saveSettings();
        if (outputName_->text().trimmed().isEmpty()) {
            QMessageBox::warning(this, tr("Name required"),
                                 tr("Enter a name for the virtual microphone."));
            return;
        }

        auto options = optionsFromUi();
        activeInputTarget_ = QString::fromStdString(options.input_target);
        selectedInputAvailable_ = true;
        // Directory discovery populates the browser only. Never let the engine
        // auto-activate unselected Linux plugins or all global bridge wrappers.
        // Headless CLI directory auto-loading remains unchanged.
        options.plugin_directories.clear();
        options.windows_plugin_directories.clear();

        previousDiagnostics_ = {};
        engineReadyShown_ = false;
        engine_ = std::make_unique<vst_host::audio::PipeWireEngine>(std::move(options));
        engineFinished_.store(false, std::memory_order_release);
        engineResult_ = 0;
        worker_ = std::thread([this] {
            engineResult_ = engine_->run();
            engineFinished_.store(true, std::memory_order_release);
        });

        startButton_->setEnabled(false);
        stopButton_->setEnabled(true);
        setEditingEnabled(false);
        editorButton_->setEnabled(false);
        status_->setText(tr("Starting audio engine with the selected plugin chain…"));
    }

    void stopAudio() {
        closeEditorDialog();
        if (engine_) engine_->request_stop();
        if (worker_.joinable()) worker_.join();
        const auto stateWarning = captureFinishedPluginStates();
        engine_.reset();
        startButton_->setEnabled(true);
        stopButton_->setEnabled(false);
        setEditingEnabled(true);
        editorButton_->setEnabled(false);
        status_->setText(stateWarning.isEmpty()
            ? tr("Audio engine stopped. Plugin settings saved.")
            : tr("Audio stopped. Some plugin settings could not be saved: %1")
                .arg(stateWarning));
        activeInputTarget_.clear();
        selectedInputAvailable_ = true;
        updateSelectedPlugin();
        saveSettings();
    }

    void closeEditorDialog() {
        if (!editorDialog_) return;
        auto* dialog = editorDialog_.data();
        editorDialog_ = nullptr;
        editorIndex_ = -1;
        dialog->detachEditor(); // release native editor before stopping the engine
        dialog->close();
    }

    void updateSelectedPlugin() {
        const auto* item = chainList_->currentItem();
        selectedPluginLabel_->setText(item
            ? tr("Selected: %1%2 — double-click to edit while running")
                .arg(displayPluginName(item->data(Qt::UserRole).toString(),
                                       item->data(Qt::UserRole + 1).toBool()))
                .arg(item->data(Qt::UserRole + 3).toByteArray().isEmpty() &&
                     item->data(Qt::UserRole + 4).toByteArray().isEmpty()
                         ? QString() : tr(" [saved settings]"))
            : tr("No effect selected"));
        const int row = chainList_->currentRow();
        const bool canReorder = !engine_ && !pluginScanPending_;
        removePluginButton_->setEnabled(canReorder && item != nullptr);
        moveUpButton_->setEnabled(canReorder && row > 0);
        moveDownButton_->setEnabled(
            canReorder && row >= 0 && row < chainList_->count() - 1);
        clearSavedStateButton_->setEnabled(item && canReorder);
        {
            QSignalBlocker blocker(bypassSelectedEffect_);
            bypassSelectedEffect_->setEnabled(item != nullptr);
            bypassSelectedEffect_->setChecked(
                item && item->data(Qt::UserRole + 2).toBool());
        }
        const bool ready = engine_ && engine_->plugin_processor() != nullptr &&
            chainList_->currentRow() >= 0 &&
            static_cast<std::size_t>(chainList_->currentRow()) < engine_->plugin_count();
        editorButton_->setEnabled(ready);
    }

    void openEditor() {
        const int selectedIndex = chainList_->currentRow();
        if (editorDialog_) {
            if (editorIndex_ == selectedIndex) {
                editorDialog_->raise();
                editorDialog_->activateWindow();
                editorDialog_->raisePluginEditor();
                return;
            }
            if (!engine_ || selectedIndex < 0) return;
            auto* next = engine_->plugin_processor(static_cast<std::size_t>(selectedIndex));
            QString error;
            if (!editorDialog_->switchProcessor(next, selectedIndex, error)) {
                // The dialog recovered the previous view; keep the main-chain
                // highlight consistent with the editor it is actually showing.
                chainList_->setCurrentRow(editorIndex_);
                QMessageBox::warning(this, tr("Could not switch plugin editor"), error);
                return;
            }
            editorIndex_ = selectedIndex;
            editorDialog_->setWindowTitle(tr("Effect %1: %2")
                .arg(selectedIndex + 1)
                .arg(displayPluginName(
                    chainList_->item(selectedIndex)->data(Qt::UserRole).toString(),
                    chainList_->item(selectedIndex)->data(Qt::UserRole + 1).toBool())));
            editorDialog_->raise();
            editorDialog_->activateWindow();
            return;
        }
        if (!engine_ || engine_->plugin_processor() == nullptr) return;
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

        QStringList effectNames;
        for (int i = 0; i < chainList_->count(); ++i) {
            auto* item = chainList_->item(i);
            effectNames << tr("%1. %2")
                .arg(i + 1)
                .arg(displayPluginName(item->data(Qt::UserRole).toString(),
                                       item->data(Qt::UserRole + 1).toBool()));
        }
        auto* dialog = new PluginEditorDialog(processor, effectNames, selectedIndex,
            [this](int index) {
                if (!engine_ || index < 0 || index >= chainList_->count()) return;
                chainList_->setCurrentRow(index);
                openEditor();
            }, this);
        if (auto* item = chainList_->item(selectedIndex))
            dialog->setWindowTitle(tr("Effect %1: %2").arg(selectedIndex + 1).arg(
                displayPluginName(item->data(Qt::UserRole).toString(),
                                  item->data(Qt::UserRole + 1).toBool())));
        QString error;
        if (!dialog->attachEditor(error)) {
            dialog->close();
            delete dialog;
            QMessageBox::warning(this, tr("Could not open plugin editor"), error);
            return;
        }
        editorDialog_ = dialog;
        editorIndex_ = selectedIndex;
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        connect(dialog, &QDialog::finished, this, [this, dialog] {
            if (editorDialog_ == dialog) {
                editorDialog_ = nullptr;
                editorIndex_ = -1;
            }
        });
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
        if (pluginScanPending_ && pluginScanFuture_.valid() &&
            pluginScanFuture_.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
            auto result = pluginScanFuture_.get();
            pluginScanPending_ = false;
            int migratedEntries = 0;
            if (result.bridge) {
                for (int i = 0; i < chainList_->count(); ++i) {
                    auto* item = chainList_->item(i);
                    if (!item->data(Qt::UserRole + 1).toBool()) continue;
                    const auto oldPath = item->data(Qt::UserRole).toString().toStdString();
                    const auto newPath = vst_host::audio::plugin_format(oldPath) ==
                            vst_host::audio::PluginFormat::Vst2
                        ? vst_host::audio::migrate_vst2_wrapper_path(oldPath, *result.bridge)
                        : vst_host::audio::detail::migrate_vst3_wrapper_path(
                            oldPath, *result.bridge);
                    if (oldPath == newPath) continue;
                    const auto path = QString::fromStdString(newPath);
                    item->setData(Qt::UserRole, path);
                    item->setText(displayPluginName(path, true));
                    item->setToolTip(path);
                    ++migratedEntries;
                }
                if (migratedEntries > 0) {
                    updateChainLabels();
                    saveSettings();
                }
            }
            const auto selected = availablePlugins_->currentItem()
                ? availablePlugins_->currentItem()->data(Qt::UserRole).toString() : QString();
            availablePlugins_->clear();
            availablePluginPaths_.clear();
            for (const auto& entry : result.plugins)
                addAvailablePlugin(QString::fromStdString(entry.first), entry.second);
            // Sort once after the bulk scan. Sorting after every insertion
            // creates quadratic UI work on large VST3 libraries.
            availablePlugins_->sortItems(Qt::AscendingOrder);
            applyPluginFilter();
            for (int i = 0; i < availablePlugins_->count(); ++i) {
                if (availablePlugins_->item(i)->data(Qt::UserRole).toString() == selected) {
                    availablePlugins_->setCurrentRow(i);
                    break;
                }
            }
            if (availablePlugins_->currentRow() < 0 && availablePlugins_->count() > 0)
                availablePlugins_->setCurrentRow(0);
            setEditingEnabled(!engine_);
            QString summary = tr("Plugin scan complete: %1 available effect(s).")
                .arg(availablePlugins_->count());
            if (result.bridge) {
                summary += tr(" Windows bridge: %1.")
                    .arg(QString::fromStdString(
                        std::string(vst_host::audio::bridge_name(result.bridge->backend))));
            }
            if (migratedEntries > 0) {
                summary += tr(" Migrated %1 saved effect path(s) to the selected bridge.")
                    .arg(migratedEntries);
            }
            if (result.duplicate_aliases > 0) {
                summary += tr(" Hidden %1 duplicate wrapper/browser alias(es).")
                    .arg(result.duplicate_aliases);
            }
            if (result.unselected_window_wrappers > 0) {
                summary += tr(" Excluded %1 bridged plugin(s) from unselected Windows directories.")
                    .arg(result.unselected_window_wrappers);
            }
            if (!result.warning.empty())
                summary += QStringLiteral("\n") + QString::fromStdString(result.warning);
            status_->setText(summary);
        }
        if (!engine_) return;
        if (engineFinished_.load(std::memory_order_acquire)) {
            const auto engineError = engine_->last_error();
            closeEditorDialog();
            if (worker_.joinable()) worker_.join();
            const auto stateWarning = captureFinishedPluginStates();
            engine_.reset();
            startButton_->setEnabled(true);
            stopButton_->setEnabled(false);
            editorButton_->setEnabled(false);
            setEditingEnabled(true);
            QString message = !engineError.empty()
                ? QString::fromStdString(engineError)
                : (engineResult_ == 0 ? tr("Audio engine stopped.")
                    : tr("Audio engine exited with an error. See terminal output."));
            if (!stateWarning.isEmpty())
                message += tr(" Plugin state warning: %1").arg(stateWarning);
            status_->setText(message);
            saveSettings();
            return;
        }
        const auto stats = engine_->diagnostics();
        // Distinguish ever-increasing cumulative totals from new loss over the
        // most recent UI polling period (normally 200 ms).
        const auto newUnderrun = stats.underrun_frames >= previousDiagnostics_.underrun_frames
            ? stats.underrun_frames - previousDiagnostics_.underrun_frames : 0;
        const auto newOverflow = stats.dropped_frames >= previousDiagnostics_.dropped_frames
            ? stats.dropped_frames - previousDiagnostics_.dropped_frames : 0;
        const auto newStale = stats.stale_frames >= previousDiagnostics_.stale_frames
            ? stats.stale_frames - previousDiagnostics_.stale_frames : 0;
        previousDiagnostics_ = stats;
        diagnostics_->setText(
            tr("Audio frames (total / last poll): missing %1 / +%2; "
               "overflow %3 / +%4; stale trimmed %5 / +%6")
                .arg(static_cast<qulonglong>(stats.underrun_frames))
                .arg(static_cast<qulonglong>(newUnderrun))
                .arg(static_cast<qulonglong>(stats.dropped_frames))
                .arg(static_cast<qulonglong>(newOverflow))
                .arg(static_cast<qulonglong>(stats.stale_frames))
                .arg(static_cast<qulonglong>(newStale)));
        if (engine_->plugin_processor() != nullptr) updateSelectedPlugin();
        if (engine_->is_running() && !engineReadyShown_) {
            // Apply any bypass changes made during asynchronous startup
            // before the plug-in list was published to the UI.
            for (int index = 0; index < chainList_->count(); ++index)
                (void)engine_->set_plugin_bypassed(
                    static_cast<std::size_t>(index),
                    chainList_->item(index)->data(Qt::UserRole + 2).toBool());
            status_->setText(chainList_->count() > 0
                ? tr("Audio is running. Select a chain effect to edit or bypass it live.")
                : tr("Audio is running without effects (direct microphone routing)."));
            engineReadyShown_ = true;
        }
        if (engine_->is_running() && !activeInputTarget_.isEmpty()) {
            const bool available = engine_->selected_input_available();
            if (available != selectedInputAvailable_) {
                selectedInputAvailable_ = available;
                status_->setText(available
                    ? tr("Selected microphone reconnected. Audio resumed: %1")
                        .arg(activeInputTarget_)
                    : tr("Waiting for selected microphone: %1. "
                         "Other microphones will not be used; virtual microphone is silent.")
                        .arg(activeInputTarget_));
            }
        }
    }

    QSplitter* workspaceSplitter_{};
    QLineEdit* pluginPath_{};
    QListWidget* availablePlugins_{};
    QSet<QString> availablePluginPaths_;
    QLineEdit* pluginSearch_{};
    QLabel* availableCount_{};
    QListWidget* chainList_{};
    QListWidget* pluginDirectories_{};
    QStringList manualPlugins_;
    std::future<PluginScanResult> pluginScanFuture_;
    bool pluginScanPending_{false};
    QPushButton* addPluginButton_{};
    QPushButton* addSelectedPluginButton_{};
    QPushButton* refreshPluginListButton_{};
    QPushButton* addDirectoryButton_{};
    QPushButton* addWindowsDirectoryButton_{};
    QPushButton* removeDirectoryButton_{};
    QPushButton* scanDirectoriesButton_{};
    QPushButton* loadConfigButton_{};
    QPushButton* saveConfigButton_{};
    QPushButton* clearSavedStateButton_{};
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
    QLabel* selectedPluginLabel_{};
    QCheckBox* bypassSelectedEffect_{};
    QLabel* status_{};
    QLabel* diagnostics_{};
    QTimer* pollTimer_{};
    std::unique_ptr<vst_host::audio::PipeWireEngine> engine_;
    std::thread worker_;
    std::future<std::pair<std::vector<vst_host::audio::PipeWireInputDevice>, std::string>> deviceScanFuture_;
    bool deviceScanPending_{false};
    vst_host::audio::AudioDiagnostics previousDiagnostics_{};
    std::atomic<bool> engineFinished_{false};
    int engineResult_{0};
    QPointer<PluginEditorDialog> editorDialog_;
    int editorIndex_{-1};
    bool engineReadyShown_{false};
    QString activeInputTarget_;
    bool selectedInputAvailable_{true};
};

} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    MainWindow window;
    window.show();
    return app.exec();
}
