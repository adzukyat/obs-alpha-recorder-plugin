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
#include <QStandardItemModel>
#include <QVBoxLayout>

#include "alpha_recorder/plugin.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("alpha_recorder_frontend", "en-US")

namespace
{

    using alpha_recorder::obs::FinalizationFormat;
    using alpha_recorder::obs::Settings;

    using RuntimeSyncFunction = bool (*)();

    RuntimeSyncFunction resolve_runtime_sync_function() noexcept
    {
        obs_module_t *module = obs_get_module("alpha_recorder");
        if (module == nullptr)
        {
            return nullptr;
        }

        void *module_lib = obs_get_module_lib(module);
        if (module_lib == nullptr)
        {
            return nullptr;
        }

#ifdef _WIN32
        return reinterpret_cast<RuntimeSyncFunction>(GetProcAddress(static_cast<HMODULE>(module_lib), "alpha_recorder_sync_runtime_hooks"));
#else
        (void)module_lib;
        return nullptr;
#endif
    }

    bool sync_runtime_hooks(QString &warning_message)
    {
        const RuntimeSyncFunction runtime_sync = resolve_runtime_sync_function();
        if (runtime_sync == nullptr)
        {
            warning_message = "Alpha Recorder saved the settings, but could not contact the recording runtime. Restart OBS if enabling does not take effect immediately.";
            blog(LOG_WARNING, "%s", warning_message.toUtf8().constData());
            return false;
        }

        if (!runtime_sync())
        {
            warning_message = "Alpha Recorder saved the settings, but the recording runtime did not accept the update. Restart OBS if enabling does not take effect immediately.";
            blog(LOG_WARNING, "%s", warning_message.toUtf8().constData());
            return false;
        }

        return true;
    }

    bool save_settings(const Settings &settings, QString &error_message, QString &warning_message)
    {
        Settings normalized_settings = settings;
        normalized_settings.finalization_format = alpha_recorder::obs::normalize_finalization_format(normalized_settings.finalization_format);

        config_t *config = obs_frontend_get_user_config();
        if (config == nullptr)
        {
            error_message = "OBS user configuration is unavailable.";
            return false;
        }

        config_set_bool(config, alpha_recorder::obs::settings_section().data(), alpha_recorder::obs::settings_enabled_key().data(), normalized_settings.enabled);
        config_set_string(config, alpha_recorder::obs::settings_section().data(), alpha_recorder::obs::settings_finalization_format_key().data(), alpha_recorder::obs::finalization_format_config_value(normalized_settings.finalization_format).data());

        if (config_save(config) != CONFIG_SUCCESS)
        {
            error_message = "Failed to save the OBS user configuration.";
            return false;
        }

        (void)sync_runtime_hooks(warning_message);

        return true;
    }

    class AlphaRecorderSettingsDialog : public QDialog
    {
    public:
        explicit AlphaRecorderSettingsDialog(QWidget *parent)
            : QDialog(parent)
        {
            const Settings settings = alpha_recorder::obs::load_settings(obs_frontend_get_user_config());

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
                QString itemText = QString::fromUtf8(option.display_name.data(), static_cast<int>(option.display_name.size()));
                const bool supported = alpha_recorder::obs::finalization_format_is_supported(option.value);
                if (!supported)
                {
                    itemText += " (unsupported)";
                }

                finalizationFormatCombo_->addItem(itemText, static_cast<int>(option.value));

                if (!supported)
                {
                    if (auto *itemModel = qobject_cast<QStandardItemModel *>(finalizationFormatCombo_->model()))
                    {
                        if (QStandardItem *item = itemModel->item(finalizationFormatCombo_->count() - 1))
                        {
                            item->setEnabled(false);
                            const std::string_view unsupported_reason = alpha_recorder::obs::finalization_format_export_unsupported_reason(option.value);
                            item->setToolTip(QString::fromUtf8(unsupported_reason.data(), static_cast<int>(unsupported_reason.size())));
                        }
                    }
                }
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
            QString warningMessage;
            if (!save_settings(collect_settings(), errorMessage, warningMessage))
            {
                QMessageBox::critical(this, "Alpha Recorder Settings", errorMessage);
                return;
            }

            if (!warningMessage.isEmpty())
            {
                QMessageBox::warning(this, "Alpha Recorder Settings", warningMessage);
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

MODULE_EXPORT const char *obs_module_description(void)
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
