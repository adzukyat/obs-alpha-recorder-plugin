#include <obs-frontend-api.h>
#include <obs-module.h>

#include <util/config-file.h>

#include <QAction>
#include <QAbstractButton>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLayout>
#include <QMainWindow>
#include <QMessageBox>
#include <QMetaObject>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <curl/curl.h>

#include <atomic>
#include <filesystem>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#include "alpha_recorder/export_worker.hpp"
#include "alpha_recorder/plugin.hpp"
#include "alpha_recorder/version.hpp"
#include "diagnostic_log.hpp"

namespace
{

    using alpha_recorder::obs::FinalizationFormat;
    using alpha_recorder::obs::Settings;

    constexpr const char *kLatestReleaseApiUrl = "https://api.github.com/repos/adzukyat/obs-alpha-recorder-plugin/releases/latest";
    constexpr const char *kTagRefsApiUrl = "https://api.github.com/repos/adzukyat/obs-alpha-recorder-plugin/git/matching-refs/tags/v";
    constexpr const char *kReleasePageUrl = "https://github.com/adzukyat/obs-alpha-recorder-plugin/releases";

    struct VersionCheckState
    {
        std::atomic_bool cancelled{false};
    };

    struct VersionTriple
    {
        int major = 0;
        int minor = 0;
        int patch = 0;
    };

    struct HttpResponse
    {
        std::string body;
        long status = 0;
    };

    struct LatestVersionResult
    {
        std::string version;
        std::string source;
    };

    bool parse_version_triple(const QString &versionText, VersionTriple &version)
    {
        QString normalized = versionText.trimmed();
        if (normalized.startsWith('v'))
        {
            normalized.remove(0, 1);
        }
        const int suffixIndex = normalized.indexOf('-');
        if (suffixIndex >= 0)
        {
            normalized.truncate(suffixIndex);
        }
        const int metadataIndex = normalized.indexOf('+');
        if (metadataIndex >= 0)
        {
            normalized.truncate(metadataIndex);
        }

        const QStringList parts = normalized.split('.');
        if (parts.size() != 3)
        {
            return false;
        }

        bool ok = false;
        const int major = parts[0].toInt(&ok);
        if (!ok)
        {
            return false;
        }
        const int minor = parts[1].toInt(&ok);
        if (!ok)
        {
            return false;
        }
        const int patch = parts[2].toInt(&ok);
        if (!ok)
        {
            return false;
        }

        version = {major, minor, patch};
        return true;
    }

    bool version_is_newer(const QString &candidateText, const QString &currentText)
    {
        VersionTriple candidate;
        VersionTriple current;
        if (!parse_version_triple(candidateText, candidate) || !parse_version_triple(currentText, current))
        {
            return false;
        }

        if (candidate.major != current.major)
        {
            return candidate.major > current.major;
        }
        if (candidate.minor != current.minor)
        {
            return candidate.minor > current.minor;
        }
        return candidate.patch > current.patch;
    }

    QString version_display_text(const QString &versionText)
    {
        QString normalized = versionText.trimmed();
        while (normalized.startsWith('v'))
        {
            normalized.remove(0, 1);
        }

        return QStringLiteral("v%1").arg(normalized);
    }

    std::size_t write_version_response(char *ptr, std::size_t size, std::size_t nmemb, void *userdata)
    {
        auto *response = static_cast<std::string *>(userdata);
        const std::size_t byteCount = size * nmemb;
        response->append(ptr, byteCount);
        return byteCount;
    }

    int version_check_progress(void *clientp, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
    {
        const auto *state = static_cast<const VersionCheckState *>(clientp);
        return state != nullptr && state->cancelled.load() ? 1 : 0;
    }

    bool json_document_from_response(const std::string &responseBody, QJsonDocument &document, std::string &errorMessage)
    {
        QJsonParseError parseError{};
        document = QJsonDocument::fromJson(QByteArray::fromStdString(responseBody), &parseError);
        if (parseError.error != QJsonParseError::NoError)
        {
            errorMessage = "GitHub version response was not valid JSON: " + parseError.errorString().toStdString();
            return false;
        }

        return true;
    }

    bool update_latest_version_candidate(const QString &candidateText, const char *source, LatestVersionResult &latest)
    {
        VersionTriple ignored;
        if (!parse_version_triple(candidateText, ignored))
        {
            return false;
        }

        const QString currentLatest = QString::fromStdString(latest.version);
        if (latest.version.empty() || version_is_newer(candidateText, currentLatest))
        {
            latest.version = candidateText.trimmed().toStdString();
            latest.source = source;
        }

        return true;
    }

    bool fetch_github_json(const char *url, HttpResponse &response, std::string &errorMessage, VersionCheckState &state)
    {
        static std::once_flag curlInitFlag;
        std::call_once(curlInitFlag, []() {
            curl_global_init(CURL_GLOBAL_DEFAULT);
        });

        CURL *curl = curl_easy_init();
        if (curl == nullptr)
        {
            errorMessage = "libcurl initialization failed";
            return false;
        }

        char errorBuffer[CURL_ERROR_SIZE] = {};
        struct curl_slist *headers = nullptr;
        headers = curl_slist_append(headers, "Accept: application/vnd.github+json");
        headers = curl_slist_append(headers, "X-GitHub-Api-Version: 2022-11-28");

        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "Alpha Recorder version check");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 8L);
        curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errorBuffer);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_version_response);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, version_check_progress);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &state);

        const CURLcode result = curl_easy_perform(curl);
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status);
        curl_easy_cleanup(curl);
        curl_slist_free_all(headers);

        if (state.cancelled.load())
        {
            errorMessage = "cancelled";
            return false;
        }
        if (result != CURLE_OK)
        {
            errorMessage = errorBuffer[0] != '\0' ? errorBuffer : curl_easy_strerror(result);
            return false;
        }
        if (response.status >= 400)
        {
            errorMessage = "GitHub API returned HTTP " + std::to_string(response.status);
            return false;
        }

        return true;
    }

    bool read_latest_release_version(LatestVersionResult &latest, std::string &errorMessage, VersionCheckState &state)
    {
        HttpResponse response;
        std::string fetchError;
        if (!fetch_github_json(kLatestReleaseApiUrl, response, fetchError, state))
        {
            if (response.status == 404)
            {
                return true;
            }

            errorMessage = fetchError;
            return false;
        }

        QJsonDocument document;
        if (!json_document_from_response(response.body, document, errorMessage) || !document.isObject())
        {
            if (errorMessage.empty())
            {
                errorMessage = "GitHub latest release response was not an object";
            }
            return false;
        }

        const QJsonObject object = document.object();
        const QString tagName = object.value(QStringLiteral("tag_name")).toString().trimmed();
        if (tagName.isEmpty())
        {
            errorMessage = "GitHub latest release response did not include tag_name";
            return false;
        }

        update_latest_version_candidate(tagName, "GitHub release", latest);
        return true;
    }

    bool read_signed_tag_candidate(const QString &tagUrl, LatestVersionResult &latest, std::string &errorMessage,
                                   VersionCheckState &state)
    {
        const QByteArray tagUrlBytes = tagUrl.toUtf8();
        HttpResponse response;
        if (!fetch_github_json(tagUrlBytes.constData(), response, errorMessage, state))
        {
            return false;
        }

        QJsonDocument document;
        if (!json_document_from_response(response.body, document, errorMessage) || !document.isObject())
        {
            if (errorMessage.empty())
            {
                errorMessage = "GitHub tag response was not an object";
            }
            return false;
        }

        const QJsonObject object = document.object();
        const QJsonObject verification = object.value(QStringLiteral("verification")).toObject();
        if (!verification.value(QStringLiteral("verified")).toBool(false))
        {
            return true;
        }

        const QString tagName = object.value(QStringLiteral("tag")).toString().trimmed();
        update_latest_version_candidate(tagName, "signed GitHub tag", latest);
        return true;
    }

    bool read_signed_tag_versions(LatestVersionResult &latest, std::string &errorMessage, VersionCheckState &state)
    {
        HttpResponse response;
        if (!fetch_github_json(kTagRefsApiUrl, response, errorMessage, state))
        {
            return false;
        }

        QJsonDocument document;
        if (!json_document_from_response(response.body, document, errorMessage) || !document.isArray())
        {
            if (errorMessage.empty())
            {
                errorMessage = "GitHub tag refs response was not an array";
            }
            return false;
        }

        for (const QJsonValue &value : document.array())
        {
            if (state.cancelled.load())
            {
                errorMessage = "cancelled";
                return false;
            }

            const QJsonObject refObject = value.toObject();
            const QJsonObject object = refObject.value(QStringLiteral("object")).toObject();
            if (object.value(QStringLiteral("type")).toString() != QStringLiteral("tag"))
            {
                continue;
            }

            const QString tagUrl = object.value(QStringLiteral("url")).toString().trimmed();
            if (tagUrl.isEmpty())
            {
                continue;
            }

            if (!read_signed_tag_candidate(tagUrl, latest, errorMessage, state))
            {
                return false;
            }
        }

        return true;
    }

    bool fetch_latest_version_text(LatestVersionResult &latest, std::string &errorMessage, VersionCheckState &state)
    {
        std::string releaseError;
        const bool releaseOk = read_latest_release_version(latest, releaseError, state);
        if (!releaseOk && state.cancelled.load())
        {
            return false;
        }

        std::string tagError;
        const bool tagOk = read_signed_tag_versions(latest, tagError, state);
        if (!tagOk && state.cancelled.load())
        {
            return false;
        }

        if (latest.version.empty())
        {
            errorMessage = "GitHub did not return a published release or verified signed v* tag";
            if (!releaseError.empty())
            {
                errorMessage += "; latest release: " + releaseError;
            }
            if (!tagError.empty())
            {
                errorMessage += "; signed tags: " + tagError;
            }
            return false;
        }

        return true;
    }

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
        normalized_settings.hevc_encoder.nvenc_gpu_index =
            alpha_recorder::obs::normalize_hevc_nvenc_gpu_index(normalized_settings.hevc_encoder.nvenc_gpu_index);
        std::string unavailableReason;
        if (!alpha_recorder::obs::finalization_format_runtime_available(normalized_settings.finalization_format, &unavailableReason))
        {
            error_message = QString::fromUtf8(unavailableReason.data(), static_cast<int>(unavailableReason.size()));
            return false;
        }
        if (normalized_settings.finalization_format == alpha_recorder::obs::FinalizationFormat::MaskHevcNvenc &&
            !alpha_recorder::obs::hevc_nvenc_encoder_settings_runtime_available(normalized_settings.hevc_encoder,
                                                                                &unavailableReason))
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
        config_set_string(config, alpha_recorder::obs::settings_section().data(),
                          alpha_recorder::obs::settings_hevc_nvenc_split_encode_key().data(),
                          alpha_recorder::obs::hevc_nvenc_split_encode_config_value(normalized_settings.hevc_encoder.nvenc_split_encode).data());
        config_set_int(config, alpha_recorder::obs::settings_section().data(),
                       alpha_recorder::obs::settings_hevc_nvenc_gpu_index_key().data(),
                       normalized_settings.hevc_encoder.nvenc_gpu_index);
        config_set_bool(config, alpha_recorder::obs::settings_section().data(),
                        alpha_recorder::obs::settings_diagnostic_logging_key().data(),
                        normalized_settings.diagnostic_logging);

        if (config_save(config) != CONFIG_SUCCESS)
        {
            error_message = "Failed to save the OBS user configuration.";
            return false;
        }

        (void)sync_runtime_hooks(warning_message);

        return true;
    }

    QString path_to_qstring(const std::filesystem::path &path)
    {
        return QString::fromStdString(path.u8string());
    }

    void show_diagnostic_log_file(QWidget *parent)
    {
        std::string errorMessage;
        if (!alpha_recorder::obs::ensure_diagnostic_log_file(&errorMessage))
        {
            QMessageBox::warning(parent, "Alpha Recorder Settings",
                                 QString::fromUtf8(errorMessage.data(), static_cast<int>(errorMessage.size())));
            return;
        }

        const std::filesystem::path logPath = alpha_recorder::obs::diagnostic_log_path();
        const QString filePath = path_to_qstring(logPath);
        bool opened = false;
#ifdef _WIN32
        opened = QProcess::startDetached(QStringLiteral("explorer.exe"),
                                         {QStringLiteral("/select,%1").arg(QDir::toNativeSeparators(filePath))});
#elif defined(__APPLE__)
        opened = QProcess::startDetached(QStringLiteral("open"), {QStringLiteral("-R"), filePath});
#else
        opened = QDesktopServices::openUrl(QUrl::fromLocalFile(path_to_qstring(logPath.parent_path())));
#endif
        if (!opened)
        {
            QMessageBox::warning(parent, "Alpha Recorder Settings", "Could not open the diagnostic log folder.");
        }
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

            versionLabel_ = new QLabel(this);
            versionLabel_->setTextFormat(Qt::RichText);
            versionLabel_->setTextInteractionFlags(Qt::TextBrowserInteraction);
            versionLabel_->setOpenExternalLinks(true);
            versionLabel_->setWordWrap(true);
            set_version_label_text("checking latest version...");
            mainLayout->addWidget(versionLabel_);
            check_latest_version();

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
            nvencGpuSpinBox_ = add_advanced_spinbox(advancedLayout, 4, "NVENC GPU", -1,
                                                    std::numeric_limits<int>::max(), "Any", &nvencGpuRowLabel_);
            splitEncodeCombo_ = add_advanced_combo(advancedLayout, 5, "Split Encode", &splitEncodeRowLabel_);
            splitEncodeCombo_->clear();
            splitEncodeCombo_->addItem("Auto", static_cast<int>(alpha_recorder::obs::HevcNvencSplitEncodeMode::Auto));
            splitEncodeCombo_->addItem("Disabled", static_cast<int>(alpha_recorder::obs::HevcNvencSplitEncodeMode::Disabled));
            splitEncodeCombo_->addItem("Forced", static_cast<int>(alpha_recorder::obs::HevcNvencSplitEncodeMode::Forced));
            splitEncodeCombo_->addItem("2 strips", static_cast<int>(alpha_recorder::obs::HevcNvencSplitEncodeMode::TwoWay));
            splitEncodeCombo_->addItem("3 strips", static_cast<int>(alpha_recorder::obs::HevcNvencSplitEncodeMode::ThreeWay));
            hevcLayout->addWidget(advancedFrame_);

            mainLayout->addWidget(hevcGroupBox_);

            auto *diagnosticsGroupBox = new QGroupBox("Diagnostics", this);
            auto *diagnosticsLayout = new QHBoxLayout(diagnosticsGroupBox);
            diagnosticsLayout->setContentsMargins(14, 10, 14, 10);
            diagnosticsLayout->setSpacing(10);
            diagnosticLogCheckBox_ = new QCheckBox("Diagnostic Log", this);
            diagnosticLogCheckBox_->setChecked(settings.diagnostic_logging);
            auto *showDiagnosticLogButton = new QPushButton("Show Log Folder", this);
            diagnosticsLayout->addWidget(diagnosticLogCheckBox_);
            diagnosticsLayout->addStretch(1);
            diagnosticsLayout->addWidget(showDiagnosticLogButton);
            mainLayout->addWidget(diagnosticsGroupBox);
            connect(showDiagnosticLogButton, &QPushButton::clicked, this, [this]() {
                show_diagnostic_log_file(this);
            });

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
            nvencGpuSpinBox_->setValue(alpha_recorder::obs::normalize_hevc_nvenc_gpu_index(settings.hevc_encoder.nvenc_gpu_index));
            select_split_encode_mode(settings.hevc_encoder.nvenc_split_encode);

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

        ~AlphaRecorderSettingsDialog() override
        {
            versionCheckState_.cancelled.store(true);
            if (versionCheckThread_.joinable())
            {
                versionCheckThread_.join();
            }
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
            settings.hevc_encoder.nvenc_split_encode = selected_split_encode_mode();
            settings.hevc_encoder.nvenc_gpu_index = nvencGpuSpinBox_->value();
            settings.diagnostic_logging = diagnosticLogCheckBox_->isChecked();

            return settings;
        }

        QString current_version_text() const
        {
            const std::string_view version = alpha_recorder::project_version();
            return QString::fromUtf8(version.data(), static_cast<int>(version.size()));
        }

        void set_version_label_text(const QString &statusText)
        {
            versionLabel_->setText(QStringLiteral("Alpha Recorder %1 - %2").arg(version_display_text(current_version_text()), statusText));
        }

        void check_latest_version()
        {
            QPointer<AlphaRecorderSettingsDialog> dialog{this};
            VersionCheckState *state = &versionCheckState_;
            versionCheckThread_ = std::thread([dialog, state]() {
                LatestVersionResult latestVersionResult;
                std::string errorMessage;
                const bool ok = fetch_latest_version_text(latestVersionResult, errorMessage, *state);
                if (state->cancelled.load() || dialog.isNull())
                {
                    return;
                }

                QMetaObject::invokeMethod(dialog.data(), [dialog, ok, latestVersionResult, errorMessage]() {
                    if (dialog.isNull())
                    {
                        return;
                    }

                    if (!ok)
                    {
                        blog(LOG_WARNING, "Alpha Recorder latest version check failed: %s", errorMessage.c_str());
                        dialog->set_version_label_text("latest version check unavailable");
                        return;
                    }

                    const QString latestVersion = QString::fromStdString(latestVersionResult.version).trimmed();
                    if (latestVersion.isEmpty())
                    {
                        blog(LOG_WARNING, "Alpha Recorder latest version check returned an empty response.");
                        dialog->set_version_label_text("latest version check unavailable");
                        return;
                    }

                    if (version_is_newer(latestVersion, dialog->current_version_text()))
                    {
                        const QString source = QString::fromStdString(latestVersionResult.source).toHtmlEscaped();
                        dialog->versionLabel_->setText(QStringLiteral("Alpha Recorder %1 - latest %2 is %3. <a href=\"%4\">Open GitHub Releases.</a>")
                                                           .arg(version_display_text(dialog->current_version_text()), source,
                                                                version_display_text(latestVersion).toHtmlEscaped(), QString::fromUtf8(kReleasePageUrl)));
                        return;
                    }

                    dialog->set_version_label_text("up to date");
                }, Qt::QueuedConnection);
            });
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

        QSpinBox *add_advanced_spinbox(QGridLayout *layout,
                                       int row,
                                       const char *label,
                                       int minimum,
                                       int maximum,
                                       const char *specialValueText,
                                       QLabel **rowLabel = nullptr)
        {
            auto *nameLabel = new QLabel(label, this);
            if (rowLabel != nullptr)
            {
                *rowLabel = nameLabel;
            }
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

        QComboBox *add_advanced_combo(QGridLayout *layout, int row, const char *label, QLabel **rowLabel = nullptr)
        {
            auto *nameLabel = new QLabel(label, this);
            if (rowLabel != nullptr)
            {
                *rowLabel = nameLabel;
            }
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

        void select_split_encode_mode(alpha_recorder::obs::HevcNvencSplitEncodeMode mode)
        {
            const int requestedValue = static_cast<int>(mode);
            for (int index = 0; index < splitEncodeCombo_->count(); ++index)
            {
                if (splitEncodeCombo_->itemData(index).toInt() == requestedValue)
                {
                    splitEncodeCombo_->setCurrentIndex(index);
                    return;
                }
            }

            splitEncodeCombo_->setCurrentIndex(0);
        }

        alpha_recorder::obs::HevcNvencSplitEncodeMode selected_split_encode_mode() const
        {
            const int currentIndex = splitEncodeCombo_->currentIndex();
            if (currentIndex >= 0)
            {
                return static_cast<alpha_recorder::obs::HevcNvencSplitEncodeMode>(splitEncodeCombo_->itemData(currentIndex).toInt());
            }

            return alpha_recorder::obs::HevcNvencSplitEncodeMode::Auto;
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
            nvencGpuRowLabel_->setVisible(nvencSelected);
            nvencGpuSpinBox_->setVisible(nvencSelected);
            nvencGpuSpinBox_->setEnabled(nvencSelected);
            splitEncodeRowLabel_->setVisible(nvencSelected);
            splitEncodeCombo_->setVisible(nvencSelected);
            splitEncodeCombo_->setEnabled(nvencSelected);
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
        QLabel *versionLabel_ = nullptr;
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
        QLabel *nvencGpuRowLabel_ = nullptr;
        QSpinBox *nvencGpuSpinBox_ = nullptr;
        QLabel *splitEncodeRowLabel_ = nullptr;
        QComboBox *splitEncodeCombo_ = nullptr;
        QCheckBox *diagnosticLogCheckBox_ = nullptr;
        VersionCheckState versionCheckState_{};
        std::thread versionCheckThread_{};
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
