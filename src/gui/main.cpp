#include "audio/pipewire_engine.hpp"
#include "audio/vst3_processor.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMessageBox>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <atomic>
#include <memory>
#include <thread>

namespace {

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
        const auto nativeId = surface_->winId();
        auto* nativeParent = reinterpret_cast<void*>(static_cast<std::uintptr_t>(nativeId));
        if (!processor_->open_editor(nativeParent, width, height, detail)) {
            error = QString::fromStdString(detail);
            return false;
        }
        attached_ = true;
        surface_->setMinimumSize(width, height);
        surface_->resize(width, height);
        resize(width + 24, height + 48);
        return true;
    }

private:
    vst_host::audio::Vst3Processor* processor_{};
    QWidget* surface_{};
    bool attached_{false};
};

class MainWindow final : public QMainWindow {
public:
    MainWindow() {
        setWindowTitle(tr("VST Host Experiment"));
        resize(680, 390);

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
        form->addRow(tr("VST3 plugin"), pluginRow);

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
    void startAudio() {
        if (worker_.joinable()) worker_.join();
        const auto pluginPath = pluginPath_->text().trimmed();
        if (pluginPath.isEmpty()) {
            QMessageBox::warning(this, tr("Plugin required"),
                                 tr("Choose a native Linux VST3 bundle first."));
            return;
        }
        if (outputName_->text().trimmed().isEmpty()) {
            QMessageBox::warning(this, tr("Name required"),
                                 tr("Enter a name for the virtual microphone."));
            return;
        }

        vst_host::audio::PipeWireOptions options;
        options.plugin_path = pluginPath.toStdString();
        options.input_target = inputTarget_->text().trimmed().toStdString();
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
        if (QGuiApplication::platformName() != QStringLiteral("xcb")) {
            QMessageBox::information(
                this, tr("X11 editor embedding required"),
                tr("The current Qt platform is '%1'. Start this application with "
                   "QT_QPA_PLATFORM=xcb in an X11/XWayland session to embed this plugin editor.")
                    .arg(QGuiApplication::platformName()));
            return;
        }
        if (!engine_) return;
        auto* processor = engine_->plugin_processor();
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
        if (engine_->plugin_processor() != nullptr) {
            editorButton_->setEnabled(true);
            status_->setText(tr("Audio is running. Plugin editor is available."));
        }
        if (engineFinished_.load(std::memory_order_acquire)) {
            if (worker_.joinable()) worker_.join();
            engine_.reset();
            startButton_->setEnabled(true);
            stopButton_->setEnabled(false);
            editorButton_->setEnabled(false);
            status_->setText(engineResult_ == 0
                                 ? tr("Audio engine stopped.")
                                 : tr("Audio engine exited with an error. See the terminal output for details."));
        }
    }

    QLineEdit* pluginPath_{};
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
