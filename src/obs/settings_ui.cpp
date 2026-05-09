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
#include <QLayout>
#include <QMainWindow>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QTimer>
#include <QVBoxLayout>

#include "alpha_recorder/export_worker.hpp"
#include "alpha_recorder/plugin.hpp"

namespace
{

    using alpha_recorder::obs::FinalizationFormat;
    using alpha_recorder::obs::Settings;

    bool sync_runtime_hooks(QString &warning_message)
    {
        if (!alpha_recorder::obs::register_runtime_hooks())
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
        config_set_string(config, alpha_recorder::obs::settings_section().data(), alpha_recorder::obs::settings_hevc_nvenc_tune_key().data(),
                          alpha_recorder::obs::hevc_nvenc_tune_config_value(normalized_settings.hevc_encoder.nvenc_tune).data());
        config_set_int(config, alpha_recorder::obs::settings_section().data(), alpha_recorder::obs::settings_hevc_gop_size_key().data(),
                       static_cast<int>(alpha_recorder::obs::clamp_hevc_gop_size(normalized_settings.hevc_encoder.gop_size)));
        config_set_int(config, alpha_recorder::obs::settings_section().data(), alpha_recorder::obs::settings_hevc_b_frames_key().data(),
                       static_cast<int>(alpha_recorder::obs::clamp_hevc_b_frames(normalized_settings.hevc_encoder.b_frames)));
        config_set_int(config, alpha_recorder::obs::settings_section().data(), alpha_recorder::obs::settings_hevc_lookahead_key().data(),
                       static_cast<int>(alpha_recorder::obs::clamp_hevc_lookahead(normalized_settings.hevc_encoder.lookahead)));
        config_set_bool(config, alpha_recorder::obs::settings_section().data(),
                        alpha_recorder::obs::settings_hevc_adaptive_quantization_key().data(),
                        normalized_settings.hevc_encoder.adaptive_quantization);

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
            mainLayout->setSizeConstraint(QLayout::SetFixedSize);
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
            qualitySpinBox_->setFixedWidth(104);
            qualityLayout->addWidget(qualitySlider_, 1);
            qualityLayout->addWidget(qualitySpinBox_);
            hevcFormLayout->addRow("Quality", qualityLayout);

            presetCombo_ = new QComboBox(this);
            presetCombo_->setMinimumWidth(165);
            hevcFormLayout->addRow("Preset", presetCombo_);

            tuneCombo_ = new QComboBox(this);
            tuneCombo_->addItem("Lossless", static_cast<int>(alpha_recorder::obs::HevcNvencTune::Lossless));
            tuneCombo_->addItem("High Quality", static_cast<int>(alpha_recorder::obs::HevcNvencTune::HighQuality));
            tuneCombo_->addItem("Low Latency", static_cast<int>(alpha_recorder::obs::HevcNvencTune::LowLatency));
            tuneCombo_->addItem("Ultra Low Latency", static_cast<int>(alpha_recorder::obs::HevcNvencTune::UltraLowLatency));
            tuneCombo_->setMinimumWidth(165);
            tuneRowLabel_ = new QLabel("Tune", this);
            hevcFormLayout->addRow(tuneRowLabel_, tuneCombo_);

            hevcLayout->addLayout(hevcFormLayout);

            advancedCheckBox_ = new QCheckBox("Advanced", this);
            hevcLayout->addWidget(advancedCheckBox_);

            advancedFrame_ = new QFrame(this);
            auto *advancedLayout = new QGridLayout(advancedFrame_);
            advancedLayout->setContentsMargins(0, 0, 0, 0);
            advancedLayout->setHorizontalSpacing(14);
            advancedLayout->setVerticalSpacing(6);
            advancedLayout->setColumnMinimumWidth(0, 120);
            advancedLayout->setColumnMinimumWidth(1, 90);
            advancedLayout->setColumnMinimumWidth(2, 120);
            advancedLayout->setColumnMinimumWidth(3, 90);
            gopSpinBox_ = add_advanced_spinbox(advancedLayout, 0, "GOP", 0, 1000, "Auto");
            bFramesSpinBox_ = add_advanced_spinbox(advancedLayout, 1, "B-frames", 0, 4, nullptr);
            lookaheadSpinBox_ = add_advanced_spinbox(advancedLayout, 2, "Lookahead", 0, 32, "Off");
            aqCombo_ = add_advanced_combo(advancedLayout, 3, "AQ");
            hevcLayout->addWidget(advancedFrame_);

            mainLayout->addWidget(hevcGroupBox_);

            select_hevc_profile(settings.hevc_encoder.quality_profile);
            populate_preset_combo(settings.finalization_format, settings.hevc_encoder.preset);
            select_hevc_preset(settings.hevc_encoder.preset);
            select_nvenc_tune(settings.hevc_encoder.nvenc_tune);
            const std::uint32_t initialCq = alpha_recorder::obs::clamp_hevc_quality_cq(settings.hevc_encoder.quality_cq);
            qualitySlider_->setValue(static_cast<int>(initialCq));
            qualitySpinBox_->setValue(static_cast<int>(initialCq));
            gopSpinBox_->setValue(static_cast<int>(alpha_recorder::obs::clamp_hevc_gop_size(settings.hevc_encoder.gop_size)));
            bFramesSpinBox_->setValue(static_cast<int>(alpha_recorder::obs::clamp_hevc_b_frames(settings.hevc_encoder.b_frames)));
            lookaheadSpinBox_->setValue(static_cast<int>(alpha_recorder::obs::clamp_hevc_lookahead(settings.hevc_encoder.lookahead)));
            aqCombo_->setCurrentIndex(settings.hevc_encoder.adaptive_quantization ? 1 : 0);

            connect(finalizationFormatCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
                populate_preset_combo(selected_finalization_format(), selected_hevc_preset());
                update_hevc_controls();
            });
            connect(profileButtonGroup_, qOverload<int>(&QButtonGroup::idClicked), this, [this](int id) {
                apply_hevc_profile_defaults(static_cast<alpha_recorder::obs::HevcQualityProfile>(id));
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
            settings.hevc_encoder.nvenc_tune = selected_nvenc_tune();
            settings.hevc_encoder.gop_size = static_cast<std::uint32_t>(gopSpinBox_->value());
            settings.hevc_encoder.b_frames = static_cast<std::uint32_t>(bFramesSpinBox_->value());
            settings.hevc_encoder.lookahead = static_cast<std::uint32_t>(lookaheadSpinBox_->value());
            settings.hevc_encoder.adaptive_quantization = aqCombo_->currentIndex() == 1;

            return settings;
        }

        FinalizationFormat selected_finalization_format() const
        {
            const int currentIndex = finalizationFormatCombo_->currentIndex();
            if (currentIndex >= 0)
            {
                return static_cast<FinalizationFormat>(finalizationFormatCombo_->itemData(currentIndex).toInt());
            }

            return FinalizationFormat::MaskPngMov;
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

        QSpinBox *add_advanced_spinbox(QGridLayout *layout, int row, const char *label, int minimum, int maximum, const char *specialValueText)
        {
            auto *nameLabel = new QLabel(label, this);
            auto *spinBox = new QSpinBox(this);
            spinBox->setRange(minimum, maximum);
            spinBox->setFixedWidth(96);
            if (specialValueText != nullptr)
            {
                spinBox->setSpecialValueText(specialValueText);
            }
            layout->addWidget(nameLabel, row / 2, (row % 2) * 2);
            layout->addWidget(spinBox, row / 2, (row % 2) * 2 + 1);
            return spinBox;
        }

        QComboBox *add_advanced_combo(QGridLayout *layout, int row, const char *label)
        {
            auto *nameLabel = new QLabel(label, this);
            auto *combo = new QComboBox(this);
            combo->addItem("Off");
            combo->addItem("On");
            combo->setFixedWidth(96);
            layout->addWidget(nameLabel, row / 2, (row % 2) * 2);
            layout->addWidget(combo, row / 2, (row % 2) * 2 + 1);
            return combo;
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

        struct HevcProfileDefaults
        {
            std::uint32_t quality_cq;
            alpha_recorder::obs::HevcEncoderPreset nvenc_preset;
            alpha_recorder::obs::HevcEncoderPreset amf_preset;
            alpha_recorder::obs::HevcNvencTune nvenc_tune;
            std::uint32_t gop_size;
            std::uint32_t b_frames;
            std::uint32_t lookahead;
            bool adaptive_quantization;
        };

        HevcProfileDefaults defaults_for_profile(alpha_recorder::obs::HevcQualityProfile profile) const
        {
            switch (profile)
            {
            case alpha_recorder::obs::HevcQualityProfile::Lossless:
                return {0U, alpha_recorder::obs::HevcEncoderPreset::NvencLossless, alpha_recorder::obs::HevcEncoderPreset::AmfQuality,
                        alpha_recorder::obs::HevcNvencTune::Lossless, 0U, 0U, 0U, false};
            case alpha_recorder::obs::HevcQualityProfile::HighQuality:
                return {19U, alpha_recorder::obs::HevcEncoderPreset::NvencP5, alpha_recorder::obs::HevcEncoderPreset::AmfQuality,
                        alpha_recorder::obs::HevcNvencTune::HighQuality, 0U, 2U, 16U, true};
            case alpha_recorder::obs::HevcQualityProfile::Balanced:
                return {23U, alpha_recorder::obs::HevcEncoderPreset::NvencP3, alpha_recorder::obs::HevcEncoderPreset::AmfBalanced,
                        alpha_recorder::obs::HevcNvencTune::HighQuality, 0U, 1U, 8U, true};
            case alpha_recorder::obs::HevcQualityProfile::Fast:
                return {28U, alpha_recorder::obs::HevcEncoderPreset::NvencP2, alpha_recorder::obs::HevcEncoderPreset::AmfSpeed,
                        alpha_recorder::obs::HevcNvencTune::LowLatency, 0U, 0U, 0U, false};
            }

            return {19U, alpha_recorder::obs::HevcEncoderPreset::NvencP3, alpha_recorder::obs::HevcEncoderPreset::AmfBalanced,
                    alpha_recorder::obs::HevcNvencTune::HighQuality, 0U, 1U, 8U, true};
        }

        void apply_hevc_profile_defaults(alpha_recorder::obs::HevcQualityProfile profile)
        {
            const HevcProfileDefaults defaults = defaults_for_profile(profile);
            qualitySpinBox_->setValue(static_cast<int>(defaults.quality_cq));
            select_hevc_preset(selected_finalization_format() == FinalizationFormat::MaskHevcAmf ? defaults.amf_preset : defaults.nvenc_preset);
            select_nvenc_tune(defaults.nvenc_tune);
            gopSpinBox_->setValue(static_cast<int>(defaults.gop_size));
            bFramesSpinBox_->setValue(static_cast<int>(defaults.b_frames));
            lookaheadSpinBox_->setValue(static_cast<int>(defaults.lookahead));
            aqCombo_->setCurrentIndex(defaults.adaptive_quantization ? 1 : 0);
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

        void select_nvenc_tune(alpha_recorder::obs::HevcNvencTune tune)
        {
            const int requestedValue = static_cast<int>(tune);
            for (int index = 0; index < tuneCombo_->count(); ++index)
            {
                if (tuneCombo_->itemData(index).toInt() == requestedValue)
                {
                    tuneCombo_->setCurrentIndex(index);
                    return;
                }
            }

            tuneCombo_->setCurrentIndex(0);
        }

        alpha_recorder::obs::HevcEncoderPreset selected_hevc_preset() const
        {
            const int currentIndex = presetCombo_->currentIndex();
            if (currentIndex >= 0)
            {
                return static_cast<alpha_recorder::obs::HevcEncoderPreset>(presetCombo_->itemData(currentIndex).toInt());
            }

            return selected_finalization_format() == FinalizationFormat::MaskHevcAmf ? alpha_recorder::obs::HevcEncoderPreset::AmfBalanced
                                                                                    : alpha_recorder::obs::HevcEncoderPreset::NvencP3;
        }

        alpha_recorder::obs::HevcNvencTune selected_nvenc_tune() const
        {
            const int currentIndex = tuneCombo_->currentIndex();
            if (currentIndex >= 0)
            {
                return static_cast<alpha_recorder::obs::HevcNvencTune>(tuneCombo_->itemData(currentIndex).toInt());
            }

            return alpha_recorder::obs::HevcNvencTune::HighQuality;
        }

        bool preset_available_for_format(FinalizationFormat format, alpha_recorder::obs::HevcEncoderPreset preset) const
        {
            switch (preset)
            {
            case alpha_recorder::obs::HevcEncoderPreset::NvencLossless:
            case alpha_recorder::obs::HevcEncoderPreset::NvencP1:
            case alpha_recorder::obs::HevcEncoderPreset::NvencP2:
            case alpha_recorder::obs::HevcEncoderPreset::NvencP3:
            case alpha_recorder::obs::HevcEncoderPreset::NvencP4:
            case alpha_recorder::obs::HevcEncoderPreset::NvencP5:
            case alpha_recorder::obs::HevcEncoderPreset::NvencP6:
            case alpha_recorder::obs::HevcEncoderPreset::NvencP7:
                return format == FinalizationFormat::MaskHevcNvenc;
            case alpha_recorder::obs::HevcEncoderPreset::AmfSpeed:
            case alpha_recorder::obs::HevcEncoderPreset::AmfBalanced:
            case alpha_recorder::obs::HevcEncoderPreset::AmfQuality:
                return format == FinalizationFormat::MaskHevcAmf;
            }

            return false;
        }

        void populate_preset_combo(FinalizationFormat format, alpha_recorder::obs::HevcEncoderPreset preferredPreset)
        {
            const QSignalBlocker blocker{presetCombo_};
            presetCombo_->clear();
            if (format == FinalizationFormat::MaskHevcAmf)
            {
                presetCombo_->addItem("Speed", static_cast<int>(alpha_recorder::obs::HevcEncoderPreset::AmfSpeed));
                presetCombo_->addItem("Balanced", static_cast<int>(alpha_recorder::obs::HevcEncoderPreset::AmfBalanced));
                presetCombo_->addItem("Quality", static_cast<int>(alpha_recorder::obs::HevcEncoderPreset::AmfQuality));
            }
            else
            {
                presetCombo_->addItem("Lossless", static_cast<int>(alpha_recorder::obs::HevcEncoderPreset::NvencLossless));
                presetCombo_->addItem("P1 - Fastest", static_cast<int>(alpha_recorder::obs::HevcEncoderPreset::NvencP1));
                presetCombo_->addItem("P2 - Faster", static_cast<int>(alpha_recorder::obs::HevcEncoderPreset::NvencP2));
                presetCombo_->addItem("P3 - Fast", static_cast<int>(alpha_recorder::obs::HevcEncoderPreset::NvencP3));
                presetCombo_->addItem("P4 - Medium", static_cast<int>(alpha_recorder::obs::HevcEncoderPreset::NvencP4));
                presetCombo_->addItem("P5 - Quality", static_cast<int>(alpha_recorder::obs::HevcEncoderPreset::NvencP5));
                presetCombo_->addItem("P6 - Higher Quality", static_cast<int>(alpha_recorder::obs::HevcEncoderPreset::NvencP6));
                presetCombo_->addItem("P7 - Slowest", static_cast<int>(alpha_recorder::obs::HevcEncoderPreset::NvencP7));
            }

            if (preset_available_for_format(format, preferredPreset))
            {
                select_hevc_preset(preferredPreset);
                return;
            }

            select_hevc_preset(defaults_for_profile(selected_hevc_profile()).amf_preset);
            if (format != FinalizationFormat::MaskHevcAmf)
            {
                select_hevc_preset(defaults_for_profile(selected_hevc_profile()).nvenc_preset);
            }
        }

        void update_hevc_controls()
        {
            const int formatIndex = finalizationFormatCombo_->currentIndex();
            const FinalizationFormat selectedFormat = selected_finalization_format();
            const bool hevcSelected = formatIndex >= 0 && selectedFormat != FinalizationFormat::MaskPngMov;
            const bool nvencSelected = selectedFormat == FinalizationFormat::MaskHevcNvenc;
            hevcGroupBox_->setVisible(hevcSelected);
            const bool qualityEnabled = hevcSelected && selected_hevc_profile() != alpha_recorder::obs::HevcQualityProfile::Lossless;
            qualitySlider_->setEnabled(qualityEnabled);
            qualitySpinBox_->setEnabled(qualityEnabled);
            presetCombo_->setEnabled(qualityEnabled);
            tuneRowLabel_->setVisible(nvencSelected);
            tuneCombo_->setVisible(nvencSelected);
            tuneCombo_->setEnabled(qualityEnabled && nvencSelected);
            gopSpinBox_->setEnabled(qualityEnabled);
            bFramesSpinBox_->setEnabled(qualityEnabled);
            lookaheadSpinBox_->setEnabled(qualityEnabled);
            aqCombo_->setEnabled(qualityEnabled);
            advancedFrame_->setVisible(hevcSelected && advancedCheckBox_->isChecked());
            QTimer::singleShot(0, this, [this]() {
                adjustSize();
            });
        }

        QCheckBox *enabledCheckBox_ = nullptr;
        QComboBox *finalizationFormatCombo_ = nullptr;
        QGroupBox *hevcGroupBox_ = nullptr;
        QButtonGroup *profileButtonGroup_ = nullptr;
        QSlider *qualitySlider_ = nullptr;
        QSpinBox *qualitySpinBox_ = nullptr;
        QComboBox *presetCombo_ = nullptr;
        QLabel *tuneRowLabel_ = nullptr;
        QComboBox *tuneCombo_ = nullptr;
        QCheckBox *advancedCheckBox_ = nullptr;
        QFrame *advancedFrame_ = nullptr;
        QSpinBox *gopSpinBox_ = nullptr;
        QSpinBox *bFramesSpinBox_ = nullptr;
        QSpinBox *lookaheadSpinBox_ = nullptr;
        QComboBox *aqCombo_ = nullptr;
    };

    QPointer<QAction> settingsAction = nullptr;

} // namespace

namespace alpha_recorder::obs
{

    void register_settings_ui() noexcept
    {
        if (!settingsAction.isNull())
        {
            return;
        }

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

    void unregister_settings_ui() noexcept
    {
        if (!settingsAction.isNull())
        {
            settingsAction->disconnect();
            settingsAction->setEnabled(false);
        }
        settingsAction.clear();
    }

} // namespace alpha_recorder::obs
