#if 0
#include <obs-frontend-api.h>
#include <obs-module.h>

#include <util/config-file.h>

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QMainWindow>
#include <QMessageBox>
#include <QVBoxLayout>

#include "alpha_recorder/plugin.hpp"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("alpha_recorder_frontend", "en-US")

namespace
{

    using alpha_recorder::obs::FinalizationFormat;
    using alpha_recorder::obs::Settings;

    Settings load_settings()
    {
        Settings settings = alpha_recorder::obs::default_settings();

        config_t *config = obs_frontend_get_user_config();
        if (config == nullptr)
        {
            return settings;
        }

        settings.enabled = config_get_bool(config, alpha_recorder::obs::settings_section().data(), alpha_recorder::obs::settings_enabled_key().data());

        const char *stored_format = config_get_string(config, alpha_recorder::obs::settings_section().data(), alpha_recorder::obs::settings_finalization_format_key().data());
        if (stored_format != nullptr)
        {
            FinalizationFormat parsed_format = settings.finalization_format;
            if (alpha_recorder::obs::try_parse_finalization_format(std::string_view{stored_format}, parsed_format))
            {
                settings.finalization_format = parsed_format;
            }
        }

        return settings;
    }

    bool save_settings(const Settings &settings, QString &error_message)
    {
        config_t *config = obs_frontend_get_user_config();
        if (config == nullptr)
        {
            error_message = "OBS user configuration is unavailable.";
            return false;
        }

        config_set_bool(config, alpha_recorder::obs::settings_section().data(), alpha_recorder::obs::settings_enabled_key().data(), settings.enabled);
        config_set_string(config, alpha_recorder::obs::settings_section().data(), alpha_recorder::obs::settings_finalization_format_key().data(), alpha_recorder::obs::finalization_format_config_value(settings.finalization_format).data());

        if (config_save(config) != CONFIG_SUCCESS)
        {
            error_message = "Failed to save the OBS user configuration.";
            return false;
        }

        return true;
    }

    class AlphaRecorderSettingsDialog : public QDialog
    {
    public:
        explicit AlphaRecorderSettingsDialog(QWidget *parent)
            : QDialog(parent)
        {
            const Settings settings = load_settings();

            setWindowTitle("Alpha Recorder Settings");
            setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);

            auto *mainLayout = new QVBoxLayout(this);
            mainLayout->setContentsMargins(12, 12, 12, 12);
            mainLayout->setSpacing(10);

            auto *introLabel = new QLabel("Configure whether alpha recording is enabled and which finalization format should be preferred.", this);
            introLabel->setWordWrap(true);
            mainLayout->addWidget(introLabel);

            enabledCheckBox_ = new QCheckBox("Enabled", this);
            enabledCheckBox_->setChecked(settings.enabled);
            mainLayout->addWidget(enabledCheckBox_);

            auto *formLayout = new QFormLayout();
            formLayout->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
            formLayout->setFormAlignment(Qt::AlignTop);
            formLayout->setHorizontalSpacing(8);
            formLayout->setVerticalSpacing(8);

            finalizationFormatCombo_ = new QComboBox(this);
            for (const auto &option : alpha_recorder::obs::finalization_format_options)
            {
                finalizationFormatCombo_->addItem(QString::fromUtf8(option.display_name.data(), static_cast<int>(option.display_name.size())), static_cast<int>(option.value));
            }

            select_finalization_format(settings.finalization_format);
            formLayout->addRow("Finalization Format", finalizationFormatCombo_);
            mainLayout->addLayout(formLayout);

            auto *buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
            connect(buttonBox, &QDialogButtonBox::accepted, this, &AlphaRecorderSettingsDialog::accept);
            connect(buttonBox, &QDialogButtonBox::rejected, this, &AlphaRecorderSettingsDialog::reject);
            mainLayout->addWidget(buttonBox);
        }

    protected:
        void accept() override
        {
            QString errorMessage;
            if (!save_settings(collect_settings(), errorMessage))
            {
                QMessageBox::critical(this, "Alpha Recorder Settings", errorMessage);
                return;
            }

            QDialog::accept();
        }

    private:
        Settings collect_settings() const
        {
            Settings settings = alpha_recorder::obs::default_settings();
            settings.enabled = enabledCheckBox_->isChecked();

            const int currentIndex = finalizationFormatCombo_->currentIndex();
            if (currentIndex >= 0)
            {
                settings.finalization_format = static_cast<FinalizationFormat>(finalizationFormatCombo_->itemData(currentIndex).toInt());
            }

            return settings;
        }

        void select_finalization_format(FinalizationFormat format)
        {
            const int requestedValue = static_cast<int>(format);
            for (int index = 0; index < finalizationFormatCombo_->count(); ++index)
            {
                if (finalizationFormatCombo_->itemData(index).toInt() == requestedValue)
                {
                    finalizationFormatCombo_->setCurrentIndex(index);
                    return;
                }
            }

            finalizationFormatCombo_->setCurrentIndex(0);
        }

        QCheckBox *enabledCheckBox_ = nullptr;
        QComboBox *finalizationFormatCombo_ = nullptr;
    };

    QAction *settingsAction = nullptr;

} // namespace

extern "C" bool obs_module_load(void)
{
    return true;
}

extern "C" const char *obs_module_description(void)
{
    return "OBS settings dialog for alpha_recorder";
}

extern "C" void obs_module_post_load(void)
{
    settingsAction = static_cast<QAction *>(obs_frontend_add_tools_menu_qaction("Alpha Recorder Settings"));
    if (settingsAction == nullptr)
    {
        return;
    }

    QObject::connect(settingsAction, &QAction::triggered, settingsAction, []()
                     {
        QMainWindow *mainWindow = static_cast<QMainWindow *>(obs_frontend_get_main_window());
        AlphaRecorderSettingsDialog dialog(mainWindow);
        dialog.exec(); });
}

extern "C" void obs_module_unload(void)
{
    if (settingsAction != nullptr)
    {
        settingsAction->disconnect();
        settingsAction->setEnabled(false);
        settingsAction = nullptr;
    }
}
#endif
#include <obs-frontend-api.h>
#include <obs-module.h>

#include <util/config-file.h>

#include <filesystem>
#include <system_error>
#include <string>

#include <QAction>
#include <QCloseEvent>
#include <QDir>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QFontDatabase>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include "alpha_recorder/e2e_scenario.hpp"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("alpha_recorder_frontend", "en-US")

namespace
{

    constexpr const char *kConfigSection = "AlphaRecorderLauncher";
    constexpr const char *kScenarioPathKey = "scenario_path";
    constexpr const char *kOutputBasePathKey = "output_base_path";
    constexpr const char *kGeometryKey = "geometry";

    enum class StatusKind
    {
        Info,
        Warning,
        Error,
        Success,
    };

    QString to_display_path(const std::filesystem::path &path)
    {
#ifdef _WIN32
        return QDir::toNativeSeparators(QString::fromStdWString(path.wstring()));
#else
        return QDir::toNativeSeparators(QString::fromStdString(path.string()));
#endif
    }

    std::filesystem::path from_text(const QString &text)
    {
#ifdef _WIN32
        return std::filesystem::path{text.toStdWString()};
#else
        return std::filesystem::path{text.toStdString()};
#endif
    }

    bool path_exists(const std::filesystem::path &path)
    {
        std::error_code error;
        return std::filesystem::exists(path, error) && !error;
    }

    bool path_is_directory(const std::filesystem::path &path)
    {
        std::error_code error;
        return std::filesystem::is_directory(path, error) && !error;
    }

    bool path_is_regular_file(const std::filesystem::path &path)
    {
        std::error_code error;
        return std::filesystem::is_regular_file(path, error) && !error;
    }

    QString load_config_text(config_t *config, const char *section, const char *key)
    {
        const char *value = config_get_string(config, section, key);
        return value != nullptr ? QString::fromUtf8(value) : QString();
    }

    void apply_status_style(QLabel *label, StatusKind kind)
    {
        switch (kind)
        {
        case StatusKind::Error:
            label->setStyleSheet("color: #c62828;");
            break;

        case StatusKind::Warning:
            label->setStyleSheet("color: #b26a00;");
            break;

        case StatusKind::Success:
            label->setStyleSheet("color: #2e7d32;");
            break;

        case StatusKind::Info:
        default:
            label->setStyleSheet(QString());
            break;
        }
    }

    class AlphaRecorderLauncherDialog : public QDialog
    {
    public:
        explicit AlphaRecorderLauncherDialog(QWidget *parent)
            : QDialog(parent)
        {
            setWindowTitle("Alpha Recorder");
            setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);

            auto *mainLayout = new QVBoxLayout();
            mainLayout->setContentsMargins(12, 12, 12, 12);
            mainLayout->setSpacing(10);

            auto *introLabel = new QLabel("Choose a scenario file and an output base folder, then start the recorder.");
            introLabel->setWordWrap(true);
            mainLayout->addWidget(introLabel);

            auto *formLayout = new QFormLayout();
            formLayout->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
            formLayout->setFormAlignment(Qt::AlignTop);
            formLayout->setHorizontalSpacing(8);
            formLayout->setVerticalSpacing(8);

            scenarioPathEdit_ = new QLineEdit(this);
            scenarioPathEdit_->setPlaceholderText("Select a scenario file");
            scenarioPathEdit_->setClearButtonEnabled(true);

            auto *scenarioBrowseButton = new QPushButton("Browse...", this);
            connect(scenarioBrowseButton, &QPushButton::clicked, this, &AlphaRecorderLauncherDialog::browseScenarioFile);

            auto *scenarioRow = new QWidget(this);
            auto *scenarioRowLayout = new QHBoxLayout(scenarioRow);
            scenarioRowLayout->setContentsMargins(0, 0, 0, 0);
            scenarioRowLayout->setSpacing(6);
            scenarioRowLayout->addWidget(scenarioPathEdit_);
            scenarioRowLayout->addWidget(scenarioBrowseButton);
            formLayout->addRow("Scenario file", scenarioRow);

            outputBaseEdit_ = new QLineEdit(this);
            outputBaseEdit_->setPlaceholderText("Select an output base folder");
            outputBaseEdit_->setClearButtonEnabled(true);

            auto *outputBrowseButton = new QPushButton("Browse...", this);
            connect(outputBrowseButton, &QPushButton::clicked, this, &AlphaRecorderLauncherDialog::browseOutputBaseFolder);

            auto *outputRow = new QWidget(this);
            auto *outputRowLayout = new QHBoxLayout(outputRow);
            outputRowLayout->setContentsMargins(0, 0, 0, 0);
            outputRowLayout->setSpacing(6);
            outputRowLayout->addWidget(outputBaseEdit_);
            outputRowLayout->addWidget(outputBrowseButton);
            formLayout->addRow("Output base folder", outputRow);

            mainLayout->addLayout(formLayout);

            statusLabel_ = new QLabel(this);
            statusLabel_->setWordWrap(true);
            statusLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
            mainLayout->addWidget(statusLabel_);

            auto *previewGroup = new QGroupBox("Launch preview", this);
            auto *previewLayout = new QVBoxLayout(previewGroup);
            previewLayout->setContentsMargins(12, 12, 12, 12);

            previewLabel_ = new QLabel(this);
            previewLabel_->setWordWrap(true);
            previewLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
            previewLabel_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
            previewLayout->addWidget(previewLabel_);
            mainLayout->addWidget(previewGroup);

            auto *buttonBox = new QDialogButtonBox(Qt::Horizontal, this);
            startButton_ = buttonBox->addButton("Start Recorder", QDialogButtonBox::AcceptRole);
            startButton_->setDefault(true);
            startButton_->setAutoDefault(true);

#include "alpha_recorder/plugin.hpp"
            connect(buttonBox, &QDialogButtonBox::accepted, this, &AlphaRecorderLauncherDialog::startRecorder);
            connect(buttonBox, &QDialogButtonBox::rejected, this, &AlphaRecorderLauncherDialog::reject);

            mainLayout->addWidget(buttonBox);
            setLayout(mainLayout);

            resize(760, 420);
            using alpha_recorder::obs::FinalizationFormat;
            using alpha_recorder::obs::Settings;
            connect(outputBaseEdit_, &QLineEdit::textChanged, this, &AlphaRecorderLauncherDialog::refreshState);
            Settings load_settings()
            {
                Settings settings = alpha_recorder::obs::default_settings();

                config_t *config = obs_frontend_get_user_config();
                if (config == nullptr)
                {
                    return settings;
                }

                settings.enabled = config_get_bool(config, alpha_recorder::obs::settings_section().data(), alpha_recorder::obs::settings_enabled_key().data());

                const char *stored_format = config_get_string(config, alpha_recorder::obs::settings_section().data(), alpha_recorder::obs::settings_finalization_format_key().data());
                if (stored_format != nullptr)
                {
                    FinalizationFormat parsed_format = settings.finalization_format;
                    if (alpha_recorder::obs::try_parse_finalization_format(std::string_view{stored_format}, parsed_format))
                    {
                        settings.finalization_format = parsed_format;
                    }
                }

                return settings;
            }

            bool save_settings(const Settings &settings, QString &error_message)
            {
                config_t *config = obs_frontend_get_user_config();
                if (config == nullptr)
                {
                    error_message = "OBS user configuration is unavailable.";
                    return false;
                }

                config_set_bool(config, alpha_recorder::obs::settings_section().data(), alpha_recorder::obs::settings_enabled_key().data(), settings.enabled);
                config_set_string(config, alpha_recorder::obs::settings_section().data(), alpha_recorder::obs::settings_finalization_format_key().data(), alpha_recorder::obs::finalization_format_config_value(settings.finalization_format).data());

                if (config_save(config) != CONFIG_SUCCESS)
                {
                    error_message = "Failed to save the OBS user configuration.";
                    return false;
                }

                return true;
            }

            class AlphaRecorderSettingsDialog : public QDialog
            {
            public:
                explicit AlphaRecorderSettingsDialog(QWidget *parent)
                    : QDialog(parent)
                {
                    const Settings settings = load_settings();

                    setWindowTitle("Alpha Recorder Settings");
                    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);

                    auto *mainLayout = new QVBoxLayout(this);
                    mainLayout->setContentsMargins(12, 12, 12, 12);
                    mainLayout->setSpacing(10);

                    auto *introLabel = new QLabel("Configure whether alpha recording is enabled and which finalization format should be preferred.", this);
                    introLabel->setWordWrap(true);
                    mainLayout->addWidget(introLabel);

                    enabledCheckBox_ = new QCheckBox("Enabled", this);
                    enabledCheckBox_->setChecked(settings.enabled);
                    mainLayout->addWidget(enabledCheckBox_);

                    auto *formLayout = new QFormLayout();
                    formLayout->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
                    formLayout->setFormAlignment(Qt::AlignTop);
                    formLayout->setHorizontalSpacing(8);
                    formLayout->setVerticalSpacing(8);

                    finalizationFormatCombo_ = new QComboBox(this);
                    for (const auto &option : alpha_recorder::obs::finalization_format_options)
                    {
                        finalizationFormatCombo_->addItem(QString::fromUtf8(option.display_name.data(), static_cast<int>(option.display_name.size())), static_cast<int>(option.value));
                    }

                    select_finalization_format(settings.finalization_format);
                    formLayout->addRow("Finalization Format", finalizationFormatCombo_);
                    mainLayout->addLayout(formLayout);

                    auto *buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
                    connect(buttonBox, &QDialogButtonBox::accepted, this, &AlphaRecorderSettingsDialog::accept);
                    connect(buttonBox, &QDialogButtonBox::rejected, this, &AlphaRecorderSettingsDialog::reject);
                    mainLayout->addWidget(buttonBox);
                }

            protected:
                void accept() override
                {
                    QString errorMessage;
                    if (!save_settings(collect_settings(), errorMessage))
                    {
                        QMessageBox::critical(this, "Alpha Recorder Settings", errorMessage);
                        return;
                    }

                    QDialog::accept();
                }

            private:
                Settings collect_settings() const
                {
                    Settings settings = alpha_recorder::obs::default_settings();
                    settings.enabled = enabledCheckBox_->isChecked();

                    const int currentIndex = finalizationFormatCombo_->currentIndex();
                    if (currentIndex >= 0)
                    {
                        settings.finalization_format = static_cast<FinalizationFormat>(finalizationFormatCombo_->itemData(currentIndex).toInt());
                    }

                    return settings;
                }

                void select_finalization_format(FinalizationFormat format)
                {
                    const int requestedValue = static_cast<int>(format);
                    for (int index = 0; index < finalizationFormatCombo_->count(); ++index)
                    {
                        if (finalizationFormatCombo_->itemData(index).toInt() == requestedValue)
                        {
                            finalizationFormatCombo_->setCurrentIndex(index);
                            return;
                        }
                    }

                    finalizationFormatCombo_->setCurrentIndex(0);
                }

                QCheckBox *enabledCheckBox_ = nullptr;
                QComboBox *finalizationFormatCombo_ = nullptr;
            };

            QAction *settingsAction = nullptr;

            if (config == nullptr)
            {
                return;
            }

            const QByteArray scenarioPath = scenarioPathEdit_->text().trimmed().toUtf8();
            const QByteArray outputBasePath = outputBaseEdit_->text().trimmed().toUtf8();
            const QByteArray geometry = saveGeometry().toBase64();

            config_set_string(config, kConfigSection, kScenarioPathKey, scenarioPath.constData());
            config_set_string(config, kConfigSection, kOutputBasePathKey, outputBasePath.constData());
            config_set_string(config, kConfigSection, kGeometryKey, geometry.constData());

            if (flush)
            {
                config_save(config);
            }
        }

    protected:
        void closeEvent(QCloseEvent *event) override
        {
            saveCurrentSettings(true);
            event->ignore();
            hide();
        }

        void reject() override
        {
            close();
        }

    private:
        void loadSettings()
        {
            config_t *config = obs_frontend_get_user_config();
            if (config == nullptr)
            {
                return;
            }

            QSignalBlocker scenarioBlocker(scenarioPathEdit_);
            QSignalBlocker outputBlocker(outputBaseEdit_);

            scenarioPathEdit_->setText(load_config_text(config, kConfigSection, kScenarioPathKey));
            outputBaseEdit_->setText(load_config_text(config, kConfigSection, kOutputBasePathKey));

            const QString geometry = load_config_text(config, kConfigSection, kGeometryKey);
            if (!geometry.isEmpty())
            {
                const QByteArray geometryBytes = QByteArray::fromBase64(geometry.toUtf8());
                restoreGeometry(geometryBytes);
            }
        }

        void setStatus(const QString &text, StatusKind kind)
        {
            statusLabel_->setText(text);
            apply_status_style(statusLabel_, kind);
        }

        void refreshState()
        {
            scenarioValid_ = false;
            outputBaseValid_ = false;
            scenario_ = alpha_recorder::e2e::E2EScenario{};
            outputBasePath_ = std::filesystem::path{};
            resolvedOutputRoot_ = std::filesystem::path{};

            const QString scenarioText = scenarioPathEdit_->text().trimmed();
            const QString outputBaseText = outputBaseEdit_->text().trimmed();

            QString previewText;
            QString statusText;
            StatusKind statusKind = StatusKind::Info;

            if (scenarioText.isEmpty())
            {
                statusText = "Select a scenario file to preview the launch.";
            }
            else
            {
                const std::filesystem::path scenarioPath = from_text(scenarioText);
                if (!path_exists(scenarioPath) || !path_is_regular_file(scenarioPath))
                {
                    statusText = QString("Scenario file not found: %1").arg(to_display_path(scenarioPath));
                    statusKind = StatusKind::Error;
                }
                else
                {
                    std::string scenarioError;
                    const std::filesystem::path scenarioPathFile = scenarioPath;
                    if (!alpha_recorder::e2e::load_scenario(scenarioPathFile, scenario_, scenarioError))
                    {
                        statusText = QString::fromStdString(scenarioError);
                        statusKind = StatusKind::Error;
                    }
                    else
                    {
                        scenarioValid_ = true;
                    }
                }
            }

            if (!scenarioValid_)
            {
                previewText = "The preview will appear once a valid scenario file and output folder are selected.";
            }

            if (outputBaseText.isEmpty())
            {
                if (statusText.isEmpty())
                {
                    statusText = scenarioValid_ ? "Select an output base folder to continue." : "Select a scenario file to preview the launch.";
                    statusKind = StatusKind::Info;
                }
            }
            else
            {
                const std::filesystem::path outputBasePath = from_text(outputBaseText);
                if (path_exists(outputBasePath) && !path_is_directory(outputBasePath))
                {
                    statusText = QString("Output base path is not a folder: %1").arg(to_display_path(outputBasePath));
                    statusKind = StatusKind::Error;
                }
                else
                {
                    outputBaseValid_ = true;
                    outputBasePath_ = outputBasePath;
                }
            }

            if (scenarioValid_ && outputBaseValid_)
            {
                resolvedOutputRoot_ = alpha_recorder::e2e::resolve_output_root(outputBasePath_, scenario_);
                const bool targetExists = path_exists(resolvedOutputRoot_);
                const bool baseExists = path_exists(outputBasePath_);

                previewText = QString("Scenario: %1\nExpected pairs: %2\nExpected drops: %3\nRelative output root: %4\nResolved output folder: %5\nArtifacts: rgb.raw, alpha.sidecar, alpha.manifest.json")
                                  .arg(QString::fromStdString(scenario_.name))
                                  .arg(scenario_.expected_pair_count)
                                  .arg(scenario_.expected_drop_count)
                                  .arg(to_display_path(scenario_.output_root))
                                  .arg(to_display_path(resolvedOutputRoot_));

                if (!baseExists)
                {
                    previewText += "\nOutput base folder will be created if missing.";
                }

                if (targetExists)
                {
                    previewText += "\nWarning: the target folder already exists and will be cleared before recording.";
                    if (statusKind != StatusKind::Error)
                    {
                        statusText = "The target folder already exists and will be cleared before recording.";
                        statusKind = StatusKind::Warning;
                    }
                }
                else if (statusKind != StatusKind::Error)
                {
                    statusText = "Ready to start.";
                    statusKind = StatusKind::Info;
                }

                startButton_->setEnabled(true);
            }
            else
            {
                startButton_->setEnabled(false);

                if (previewText.isEmpty())
                {
                    previewText = "The preview will appear once a valid scenario file and output folder are selected.";
                }
            }

            if (!scenarioValid_ && !scenarioText.isEmpty() && statusKind != StatusKind::Error)
            {
                statusText = "Select a valid scenario file.";
                statusKind = StatusKind::Error;
            }

            if (!outputBaseValid_ && !outputBaseText.isEmpty() && statusKind != StatusKind::Error)
            {
                statusText = "Select a valid output base folder.";
                statusKind = StatusKind::Error;
            }

            setStatus(statusText, statusKind);
            previewLabel_->setText(previewText);
        }

        void browseScenarioFile()
        {
            QString startDirectory;
            const QString currentText = scenarioPathEdit_->text().trimmed();
            if (!currentText.isEmpty())
            {
                const QFileInfo info(currentText);
                startDirectory = info.exists() ? info.absolutePath() : info.path();
            }

            if (startDirectory.isEmpty())
            {
                const QString outputText = outputBaseEdit_->text().trimmed();
                if (!outputText.isEmpty())
                {
                    const QFileInfo outputInfo(outputText);
                    startDirectory = outputInfo.exists() ? outputInfo.absolutePath() : outputInfo.path();
                }
            }

            if (startDirectory.isEmpty())
            {
                startDirectory = QDir::homePath();
            }

            const QString path = QFileDialog::getOpenFileName(this, "Select Scenario File", startDirectory, "Scenario files (*.scenario);;All files (*)");
            if (!path.isEmpty())
            {
                scenarioPathEdit_->setText(path);
            }
        }

        void browseOutputBaseFolder()
        {
            QString startDirectory;
            const QString currentText = outputBaseEdit_->text().trimmed();
            if (!currentText.isEmpty())
            {
                const QFileInfo info(currentText);
                startDirectory = info.exists() ? info.absolutePath() : info.path();
            }

            if (startDirectory.isEmpty())
            {
                const QString scenarioText = scenarioPathEdit_->text().trimmed();
                if (!scenarioText.isEmpty())
                {
                    const QFileInfo scenarioInfo(scenarioText);
                    startDirectory = scenarioInfo.exists() ? scenarioInfo.absolutePath() : scenarioInfo.path();
                }
            }

            if (startDirectory.isEmpty())
            {
                startDirectory = QDir::homePath();
            }

            const QString path = QFileDialog::getExistingDirectory(this, "Select Output Base Folder", startDirectory);
            if (!path.isEmpty())
            {
                outputBaseEdit_->setText(path);
            }
        }

        void startRecorder()
        {
            refreshState();
            if (!scenarioValid_ || !outputBaseValid_)
            {
                return;
            }

            if (path_exists(resolvedOutputRoot_))
            {
                const QString prompt = QString("The target folder already exists and will be cleared:\n\n%1\n\nContinue?").arg(to_display_path(resolvedOutputRoot_));
                const QMessageBox::StandardButton decision = QMessageBox::warning(this, "Alpha Recorder", prompt, QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
                if (decision != QMessageBox::Yes)
                {
                    return;
                }
            }

            saveCurrentSettings(true);

            obs_data_t *settings = obs_data_create();
            const QByteArray scenarioPathUtf8 = scenarioPathEdit_->text().trimmed().toUtf8();
            const QByteArray outputBaseUtf8 = outputBaseEdit_->text().trimmed().toUtf8();
            obs_data_set_string(settings, "scenario_path", scenarioPathUtf8.constData());
            obs_data_set_string(settings, "artifact_root", outputBaseUtf8.constData());

            obs_output_t *output = obs_output_create("alpha_recorder_output", "alpha_recorder_gui", settings, nullptr);
            obs_data_release(settings);

            if (output == nullptr)
            {
                const QString message = "Failed to create the Alpha Recorder output.";
                setStatus(message, StatusKind::Error);
                QMessageBox::critical(this, "Alpha Recorder", message);
                return;
            }

            const bool started = obs_output_start(output);
            if (!started)
            {
                const char *lastError = obs_output_get_last_error(output);
                const QString message = lastError != nullptr && *lastError != '\0' ? QString::fromUtf8(lastError) : QString("Alpha Recorder failed to start.");
                setStatus(message, StatusKind::Error);
                QMessageBox::critical(this, "Alpha Recorder", message);
                obs_output_release(output);
                return;
            }

            obs_output_stop(output);
            obs_output_release(output);

            const QString message = QString("Recorder completed successfully. Output written to %1.").arg(to_display_path(resolvedOutputRoot_));
            setStatus(message, StatusKind::Success);
            saveCurrentSettings(true);
        }

        QLineEdit *scenarioPathEdit_ = nullptr;
        QLineEdit *outputBaseEdit_ = nullptr;
        QLabel *statusLabel_ = nullptr;
        QLabel *previewLabel_ = nullptr;
        QPushButton *startButton_ = nullptr;
        alpha_recorder::e2e::E2EScenario scenario_{};
        std::filesystem::path outputBasePath_{};
        std::filesystem::path resolvedOutputRoot_{};
        bool scenarioValid_ = false;
        bool outputBaseValid_ = false;
    };

    AlphaRecorderLauncherDialog *launcherDialog = nullptr;
    QPointer<QAction> launcherAction = nullptr;

    void save_launcher_settings(obs_data_t *, bool saving, void *)
    {
        if (!saving || launcherDialog == nullptr)
        {
            return;
        }

        launcherDialog->saveCurrentSettings(false);
    }

} // namespace

extern "C" bool obs_module_load(void)
{
    return true;
}

extern "C" const char *obs_module_description(void)
{
    return "OBS launcher UI for alpha_recorder";
}

extern "C" void obs_module_post_load(void)
{
    launcherAction = static_cast<QAction *>(obs_frontend_add_tools_menu_qaction("Alpha Recorder"));
    if (launcherAction == nullptr)
    {
        return;
    }

    obs_frontend_add_save_callback(save_launcher_settings, nullptr);

    QObject::connect(launcherAction, &QAction::triggered, launcherAction, []()
                     {
        QMainWindow *mainWindow = static_cast<QMainWindow *>(obs_frontend_get_main_window());
        if (launcherDialog == nullptr)
        {
            launcherDialog = new AlphaRecorderLauncherDialog(mainWindow);
        }

        if (!launcherDialog->isVisible())
        {
            launcherDialog->show();
        }

        launcherDialog->raise();
        launcherDialog->activateWindow(); });
}

extern "C" void obs_module_unload(void)
{
    if (launcherDialog != nullptr)
    {
        launcherDialog->saveCurrentSettings(true);
    }

    obs_frontend_remove_save_callback(save_launcher_settings, nullptr);

    if (!launcherAction.isNull())
    {
        launcherAction->disconnect();
        launcherAction->setEnabled(false);
    }
    launcherAction.clear();

    if (launcherDialog != nullptr)
    {
        delete launcherDialog;
        launcherDialog = nullptr;
    }
}
