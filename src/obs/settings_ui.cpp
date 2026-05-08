#include <obs-frontend-api.h>
#include <obs-module.h>

#include <util/config-file.h>

#include <QAction>
#include <QAbstractButton>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMainWindow>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QVBoxLayout>

#include "alpha_recorder/export_worker.hpp"
#include "alpha_recorder/plugin.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#else
#include <dlfcn.h>
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
        return reinterpret_cast<RuntimeSyncFunction>(dlsym(module_lib, "alpha_recorder_sync_runtime_hooks"));
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
        std::string unavailableReason;
        if (!alpha_recorder::obs::finalization_format_runtime_available(normalized_settings.finalization_format, &unavailableReason))
        {
            error_message = QString::fromUtf8(unavailableReason.data(), static_cast<int>(unavailableReason.size()));
            return false;
        }

        config_t *config = obs_frontend_get_user_config();
        if (config == nullptr)
        {
            error_message = "OBS user configuration is unavailable.";
            return false;
        }

        config_set_bool(config, alpha_recorder::obs::settings_section().data(), alpha_recorder::obs::settings_enabled_key().data(), normalized_settings.enabled);
        config_set_string(config, alpha_recorder::obs::settings_section().data(), alpha_recorder::obs::settings_finalization_format_key().data(), alpha_recorder::obs::finalization_format_config_value(normalized_settings.finalization_format).data());
        config_set_string(config, alpha_recorder::obs::settings_section().data(), alpha_recorder::obs::settings_hevc_quality_profile_key().data(),
                          alpha_recorder::obs::hevc_quality_profile_config_value(normalized_settings.hevc_encoder.quality_profile).data());
        config_set_int(config, alpha_recorder::obs::settings_section().data(), alpha_recorder::obs::settings_hevc_quality_cq_key().data(),
                       static_cast<int>(alpha_recorder::obs::clamp_hevc_quality_cq(normalized_settings.hevc_encoder.quality_cq)));
        config_set_string(config, alpha_recorder::obs::settings_section().data(), alpha_recorder::obs::settings_hevc_preset_key().data(),
                          alpha_recorder::obs::hevc_encoder_preset_config_value(normalized_settings.hevc_encoder.preset).data());

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
            setMinimumWidth(560);

            auto *mainLayout = new QVBoxLayout(this);
            mainLayout->setContentsMargins(18, 16, 18, 16);
            mainLayout->setSpacing(12);

            auto *introLabel = new QLabel("Configure alpha recording and tune the mask encoder used for HEVC outputs.", this);
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
            finalizationFormatCombo_->setMinimumWidth(250);
            for (const auto &option : alpha_recorder::obs::finalization_format_options)
            {
                QString itemText = QString::fromUtf8(option.display_name.data(), static_cast<int>(option.display_name.size()));
                std::string unsupportedReason;
                const bool supported = alpha_recorder::obs::finalization_format_runtime_available(option.value, &unsupportedReason);
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
                            item->setToolTip(QString::fromUtf8(unsupportedReason.data(), static_cast<int>(unsupportedReason.size())));
                        }
                    }
                }
            }

            select_finalization_format(settings.finalization_format);
            formLayout->addRow("Finalization Format", finalizationFormatCombo_);
            mainLayout->addLayout(formLayout);

            hevcGroupBox_ = new QGroupBox("HEVC Encoder", this);
            hevcGroupBox_->setFlat(false);
            auto *hevcLayout = new QVBoxLayout(hevcGroupBox_);
            hevcLayout->setContentsMargins(14, 14, 14, 12);
            hevcLayout->setSpacing(11);

            profileButtonGroup_ = new QButtonGroup(this);
            profileButtonGroup_->setExclusive(true);
            auto *profileLayout = new QHBoxLayout();
            profileLayout->setSpacing(6);
            add_profile_button(profileLayout, "Lossless", alpha_recorder::obs::HevcQualityProfile::Lossless);
            add_profile_button(profileLayout, "High Quality", alpha_recorder::obs::HevcQualityProfile::HighQuality);
            add_profile_button(profileLayout, "Balanced", alpha_recorder::obs::HevcQualityProfile::Balanced);
            add_profile_button(profileLayout, "Fast", alpha_recorder::obs::HevcQualityProfile::Fast);
            hevcLayout->addLayout(profileLayout);

            auto *hevcFormLayout = new QFormLayout();
            hevcFormLayout->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
            hevcFormLayout->setFormAlignment(Qt::AlignTop);
            hevcFormLayout->setHorizontalSpacing(10);
            hevcFormLayout->setVerticalSpacing(8);

            auto *qualityLayout = new QHBoxLayout();
            qualityLayout->setSpacing(8);
            qualitySlider_ = new QSlider(Qt::Horizontal, this);
            qualitySlider_->setRange(0, 51);
            qualitySlider_->setTickInterval(1);
            qualitySlider_->setMinimumWidth(260);
            qualitySpinBox_ = new QSpinBox(this);
            qualitySpinBox_->setRange(0, 51);
            qualitySpinBox_->setPrefix("CQ ");
            qualitySpinBox_->setFixedWidth(82);
            qualityLayout->addWidget(qualitySlider_, 1);
            qualityLayout->addWidget(qualitySpinBox_);
            hevcFormLayout->addRow("Quality", qualityLayout);

            presetCombo_ = new QComboBox(this);
            presetCombo_->addItem("Fast", static_cast<int>(alpha_recorder::obs::HevcEncoderPreset::Fast));
            presetCombo_->addItem("P3 - Balanced", static_cast<int>(alpha_recorder::obs::HevcEncoderPreset::Balanced));
            presetCombo_->addItem("Quality", static_cast<int>(alpha_recorder::obs::HevcEncoderPreset::Quality));
            presetCombo_->setMinimumWidth(165);
            hevcFormLayout->addRow("Preset", presetCombo_);

            auto *rateControlCombo = new QComboBox(this);
            rateControlCombo->addItem("VBR");
            rateControlCombo->setMinimumWidth(165);
            rateControlCombo->setToolTip("NVENC uses VBR with CQ. AMF maps the same quality value to constant QP.");
            hevcFormLayout->addRow("Rate Control", rateControlCombo);
            hevcLayout->addLayout(hevcFormLayout);

            advancedCheckBox_ = new QCheckBox("Advanced", this);
            hevcLayout->addWidget(advancedCheckBox_);

            advancedFrame_ = new QFrame(this);
            auto *advancedLayout = new QGridLayout(advancedFrame_);
            advancedLayout->setContentsMargins(0, 0, 0, 0);
            advancedLayout->setHorizontalSpacing(14);
            advancedLayout->setVerticalSpacing(6);
            add_advanced_readout(advancedLayout, 0, "GOP", "FPS");
            add_advanced_readout(advancedLayout, 1, "B-frames", "0");
            add_advanced_readout(advancedLayout, 2, "Lookahead", "Off");
            add_advanced_readout(advancedLayout, 3, "AQ", "Off");
            hevcLayout->addWidget(advancedFrame_);

            mainLayout->addWidget(hevcGroupBox_);

            select_hevc_profile(settings.hevc_encoder.quality_profile);
            select_hevc_preset(settings.hevc_encoder.preset);
            const std::uint32_t initialCq = alpha_recorder::obs::clamp_hevc_quality_cq(settings.hevc_encoder.quality_cq);
            qualitySlider_->setValue(static_cast<int>(initialCq));
            qualitySpinBox_->setValue(static_cast<int>(initialCq));

            connect(finalizationFormatCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) { update_hevc_controls(); });
            connect(profileButtonGroup_, qOverload<int>(&QButtonGroup::idClicked), this, [this](int id) {
                qualitySpinBox_->setValue(static_cast<int>(default_cq_for_profile(static_cast<alpha_recorder::obs::HevcQualityProfile>(id))));
                update_hevc_controls();
            });
            connect(qualitySlider_, &QSlider::valueChanged, qualitySpinBox_, &QSpinBox::setValue);
            connect(qualitySpinBox_, qOverload<int>(&QSpinBox::valueChanged), qualitySlider_, &QSlider::setValue);
            connect(advancedCheckBox_, &QCheckBox::toggled, advancedFrame_, &QFrame::setVisible);

            update_hevc_controls();

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

            settings.hevc_encoder.quality_profile = selected_hevc_profile();
            settings.hevc_encoder.quality_cq = static_cast<std::uint32_t>(qualitySpinBox_->value());
            settings.hevc_encoder.preset = selected_hevc_preset();

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

        void add_profile_button(QHBoxLayout *layout, const char *text, alpha_recorder::obs::HevcQualityProfile profile)
        {
            auto *button = new QPushButton(text, this);
            button->setCheckable(true);
            button->setMinimumHeight(28);
            profileButtonGroup_->addButton(button, static_cast<int>(profile));
            layout->addWidget(button);
        }

        void add_advanced_readout(QGridLayout *layout, int row, const char *label, const char *value)
        {
            auto *nameLabel = new QLabel(label, this);
            auto *valueLabel = new QLabel(value, this);
            valueLabel->setEnabled(false);
            layout->addWidget(nameLabel, row / 2, (row % 2) * 2);
            layout->addWidget(valueLabel, row / 2, (row % 2) * 2 + 1);
        }

        void select_hevc_profile(alpha_recorder::obs::HevcQualityProfile profile)
        {
            if (QAbstractButton *button = profileButtonGroup_->button(static_cast<int>(profile)))
            {
                button->setChecked(true);
                return;
            }

            if (QAbstractButton *button = profileButtonGroup_->button(static_cast<int>(alpha_recorder::obs::HevcQualityProfile::HighQuality)))
            {
                button->setChecked(true);
            }
        }

        alpha_recorder::obs::HevcQualityProfile selected_hevc_profile() const
        {
            const int checkedId = profileButtonGroup_->checkedId();
            if (checkedId >= 0)
            {
                return static_cast<alpha_recorder::obs::HevcQualityProfile>(checkedId);
            }

            return alpha_recorder::obs::HevcQualityProfile::HighQuality;
        }

        std::uint32_t default_cq_for_profile(alpha_recorder::obs::HevcQualityProfile profile) const
        {
            switch (profile)
            {
            case alpha_recorder::obs::HevcQualityProfile::Lossless:
                return 0U;
            case alpha_recorder::obs::HevcQualityProfile::HighQuality:
                return 19U;
            case alpha_recorder::obs::HevcQualityProfile::Balanced:
                return 23U;
            case alpha_recorder::obs::HevcQualityProfile::Fast:
                return 28U;
            }

            return 19U;
        }

        void select_hevc_preset(alpha_recorder::obs::HevcEncoderPreset preset)
        {
            const int requestedValue = static_cast<int>(preset);
            for (int index = 0; index < presetCombo_->count(); ++index)
            {
                if (presetCombo_->itemData(index).toInt() == requestedValue)
                {
                    presetCombo_->setCurrentIndex(index);
                    return;
                }
            }

            presetCombo_->setCurrentIndex(1);
        }

        alpha_recorder::obs::HevcEncoderPreset selected_hevc_preset() const
        {
            const int currentIndex = presetCombo_->currentIndex();
            if (currentIndex >= 0)
            {
                return static_cast<alpha_recorder::obs::HevcEncoderPreset>(presetCombo_->itemData(currentIndex).toInt());
            }

            return alpha_recorder::obs::HevcEncoderPreset::Balanced;
        }

        void update_hevc_controls()
        {
            const int formatIndex = finalizationFormatCombo_->currentIndex();
            const bool hevcSelected = formatIndex >= 0 &&
                                      static_cast<FinalizationFormat>(finalizationFormatCombo_->itemData(formatIndex).toInt()) !=
                                          FinalizationFormat::MaskPngMov;
            hevcGroupBox_->setVisible(hevcSelected);
            const bool qualityEnabled = hevcSelected && selected_hevc_profile() != alpha_recorder::obs::HevcQualityProfile::Lossless;
            qualitySlider_->setEnabled(qualityEnabled);
            qualitySpinBox_->setEnabled(qualityEnabled);
            presetCombo_->setEnabled(qualityEnabled);
            advancedFrame_->setVisible(hevcSelected && advancedCheckBox_->isChecked());
        }

        QCheckBox *enabledCheckBox_ = nullptr;
        QComboBox *finalizationFormatCombo_ = nullptr;
        QGroupBox *hevcGroupBox_ = nullptr;
        QButtonGroup *profileButtonGroup_ = nullptr;
        QSlider *qualitySlider_ = nullptr;
        QSpinBox *qualitySpinBox_ = nullptr;
        QComboBox *presetCombo_ = nullptr;
        QCheckBox *advancedCheckBox_ = nullptr;
        QFrame *advancedFrame_ = nullptr;
    };

    QPointer<QAction> settingsAction = nullptr;

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
    if (!settingsAction.isNull())
    {
        settingsAction->disconnect();
        settingsAction->setEnabled(false);
    }
    settingsAction.clear();
}
