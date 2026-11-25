/*
OpenAVX for OBS Studio - Qt Output Settings Dialog
Copyright (C) 2026 Saif Mukhtar <saifmukhtar20@gmail.com>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "openavx-output-dialog.hpp"
#include "openavx-output-controller.h"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <openavx.h>

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cstring>

namespace {

QPointer<OpenAVXOutputDialog> g_output_dialog;
bool g_ui_exit_hooked = false;

QString obsText(const char *key)
{
	const char *val = obs_module_text(key);
	return QString::fromUtf8(val ? val : key);
}

QString formatByteCount(uint64_t bytes)
{
	if (bytes < 1024ULL)
		return QString::number(bytes) + " B";
	double val = static_cast<double>(bytes) / 1024.0;
	if (val < 1024.0)
		return QString::number(val, 'f', 1) + " KiB";
	val /= 1024.0;
	if (val < 1024.0)
		return QString::number(val, 'f', 2) + " MiB";
	val /= 1024.0;
	return QString::number(val, 'f', 2) + " GiB";
}

void copyQStringToBuf(const QString &src, char *dst, size_t cap)
{
	if (!dst || cap == 0)
		return;
	QByteArray utf8 = src.trimmed().toUtf8();
	size_t n = std::min(static_cast<size_t>(utf8.size()), cap - 1);
	if (n > 0)
		std::memcpy(dst, utf8.constData(), n);
	dst[n] = '\0';
}

void selectComboData(QComboBox *combo, int target)
{
	if (!combo)
		return;
	int idx = combo->findData(target);
	if (idx >= 0)
		combo->setCurrentIndex(idx);
	else if (combo->count() > 0)
		combo->setCurrentIndex(0);
}

void closeDialogOnUi()
{
	if (!g_output_dialog.isNull()) {
		OpenAVXOutputDialog *dlg = g_output_dialog.data();
		g_output_dialog.clear();
		dlg->close();
		delete dlg;
	}
}

void onFrontendUiEvent(enum obs_frontend_event event, void *private_data)
{
	UNUSED_PARAMETER(private_data);
	if (event == OBS_FRONTEND_EVENT_EXIT)
		closeDialogOnUi();
}

void onToolsMenuClicked(void *private_data)
{
	UNUSED_PARAMETER(private_data);
	if (g_output_dialog.isNull()) {
		QWidget *mainWin = static_cast<QWidget *>(obs_frontend_get_main_window());
		g_output_dialog = new OpenAVXOutputDialog(mainWin);
		g_output_dialog->setAttribute(Qt::WA_DeleteOnClose, true);
	} else {
		g_output_dialog->reloadFromController();
	}

	g_output_dialog->show();
	g_output_dialog->raise();
	g_output_dialog->activateWindow();
}

} // namespace

OpenAVXOutputDialog::OpenAVXOutputDialog(QWidget *parent) : QDialog(parent)
{
	setWindowTitle(obsText("OpenAVX.Dialog.Title"));
	setMinimumWidth(560);
	resize(580, 480);

	auto *root = new QVBoxLayout(this);
	root->setContentsMargins(10, 10, 10, 10);
	root->setSpacing(10);

	auto *tabs = new QTabWidget(this);

	/* === Tab 1: General === */
	auto *generalTab = new QWidget(tabs);
	auto *generalLayout = new QVBoxLayout(generalTab);
	generalLayout->setSpacing(10);

	/* Main Output Group */
	auto *mainGroup = new QGroupBox(obsText("OpenAVX.Dialog.MainGroup"), generalTab);
	auto *mainLayout = new QVBoxLayout(mainGroup);
	mainLayout->setSpacing(6);

	m_mainEnabled = new QCheckBox(obsText("OpenAVX.Dialog.EnableMain"), mainGroup);
	mainLayout->addWidget(m_mainEnabled);

	auto *mainForm = new QFormLayout();
	mainForm->setVerticalSpacing(6);
	m_mainName = new QLineEdit(mainGroup);
	m_mainName->setMaxLength(250);
	m_mainName->setPlaceholderText(obsText("OpenAVX.Dialog.MainNamePlaceholder"));

	m_mainStatus = new QLabel(mainGroup);
	m_mainStatus->setWordWrap(true);

	mainForm->addRow(obsText("OpenAVX.Output.StreamName") + ":", m_mainName);
	mainForm->addRow(obsText("OpenAVX.Dialog.Status") + ":", m_mainStatus);
	mainLayout->addLayout(mainForm);
	generalLayout->addWidget(mainGroup);

	/* Preview Output Group */
	auto *previewGroup = new QGroupBox(obsText("OpenAVX.Dialog.PreviewGroup"), generalTab);
	auto *previewLayout = new QVBoxLayout(previewGroup);
	previewLayout->setSpacing(6);

	m_previewEnabled = new QCheckBox(obsText("OpenAVX.Dialog.EnablePreview"), previewGroup);
	previewLayout->addWidget(m_previewEnabled);

	auto *previewForm = new QFormLayout();
	previewForm->setVerticalSpacing(6);
	m_previewName = new QLineEdit(previewGroup);
	m_previewName->setMaxLength(250);
	m_previewName->setPlaceholderText(obsText("OpenAVX.Dialog.PreviewNamePlaceholder"));
	m_previewIncludeAudio = new QCheckBox(obsText("OpenAVX.Dialog.PreviewAudio"), previewGroup);
	m_previewStatus = new QLabel(previewGroup);
	m_previewStatus->setWordWrap(true);

	previewForm->addRow(obsText("OpenAVX.Output.StreamName") + ":", m_previewName);
	previewForm->addRow(QString(), m_previewIncludeAudio);
	previewForm->addRow(obsText("OpenAVX.Dialog.Status") + ":", m_previewStatus);
	previewLayout->addLayout(previewForm);
	generalLayout->addWidget(previewGroup);

	/* Security Group */
	auto *secGroup = new QGroupBox(obsText("OpenAVX.Dialog.SecurityGroup"), generalTab);
	auto *secForm = new QFormLayout(secGroup);
	secForm->setVerticalSpacing(6);
	m_passphrase = new QLineEdit(secGroup);
	m_passphrase->setEchoMode(QLineEdit::Password);
	m_passphrase->setMaxLength(250);
	m_passphrase->setPlaceholderText(obsText("OpenAVX.Output.PassphrasePlaceholder"));
	secForm->addRow(obsText("OpenAVX.Output.Passphrase") + ":", m_passphrase);
	generalLayout->addWidget(secGroup);
	generalLayout->addStretch(1);

	tabs->addTab(generalTab, obsText("OpenAVX.Dialog.Tab.General"));

	/* === Tab 2: Encoding === */
	auto *encodingTab = new QWidget(tabs);
	auto *encodingLayout = new QVBoxLayout(encodingTab);
	encodingLayout->setSpacing(10);

	auto *encGroup = new QGroupBox(obsText("OpenAVX.Dialog.EncodingGroup"), encodingTab);
	m_encForm = new QFormLayout(encGroup);
	m_encForm->setVerticalSpacing(8);

	m_videoComp = new QComboBox(encGroup);
	m_videoComp->addItem(obsText("OpenAVX.Output.VideoCompression.Uncompressed"), 0);
	m_videoComp->addItem(obsText("OpenAVX.Output.VideoCompression.SpeedHQ"), 1);
	m_videoComp->addItem(obsText("OpenAVX.Output.VideoCompression.H264"), 2);
	m_videoComp->addItem(obsText("OpenAVX.Output.VideoCompression.HEVC"), 3);
	m_videoComp->addItem(obsText("OpenAVX.Output.VideoCompression.AV1"), 4);

	m_qualityRowWidget = new QWidget(encGroup);
	auto *qualityRow = new QHBoxLayout(m_qualityRowWidget);
	qualityRow->setContentsMargins(0, 0, 0, 0);
	m_speedhqQuality = new QSlider(Qt::Horizontal, m_qualityRowWidget);
	m_speedhqQuality->setRange(20, 98);
	m_speedhqQuality->setSingleStep(2);
	m_speedhqQuality->setPageStep(4);
	m_speedhqQualityVal = new QLabel(QStringLiteral("96"), m_qualityRowWidget);
	m_speedhqQualityVal->setMinimumWidth(28);
	m_speedhqQualityVal->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
	qualityRow->addWidget(m_speedhqQuality, 1);
	qualityRow->addWidget(m_speedhqQualityVal);

	m_bitrateKbps = new QSpinBox(encGroup);
	m_bitrateKbps->setRange(1000, 50000);
	m_bitrateKbps->setSingleStep(500);
	m_bitrateKbps->setSuffix(QStringLiteral(" kbps"));

	m_keyframeInterval = new QSpinBox(encGroup);
	m_keyframeInterval->setRange(15, 300);
	m_keyframeInterval->setSingleStep(1);
	m_keyframeInterval->setSuffix(QStringLiteral(" frames"));

	m_colorFormat = new QComboBox(encGroup);
	m_colorFormat->addItem(obsText("OpenAVX.Output.ColorFormat.Auto"), OAVX_COLOR_FORMAT_BEST);
	m_colorFormat->addItem(obsText("OpenAVX.Output.ColorFormat.UYVY"), OAVX_COLOR_FORMAT_UYVY_BGRA);
	m_colorFormat->addItem(obsText("OpenAVX.Output.ColorFormat.BGRA"), OAVX_COLOR_FORMAT_BGRX_BGRA);
	m_colorFormat->addItem(obsText("OpenAVX.Output.ColorFormat.RGBA"), OAVX_COLOR_FORMAT_RGBX_RGBA);

	m_audioComp = new QComboBox(encGroup);
	m_audioComp->addItem(obsText("OpenAVX.Output.AudioCompression.Off"), OAVX_AUDIO_COMPRESSION_OFF);
	m_audioComp->addItem(obsText("OpenAVX.Output.AudioCompression.Adaptive"), OAVX_AUDIO_COMPRESSION_ADAPTIVE);
	m_audioComp->addItem(obsText("OpenAVX.Output.AudioCompression.Manual"), OAVX_AUDIO_COMPRESSION_MANUAL);

	m_encForm->addRow(obsText("OpenAVX.Output.VideoCompression") + ":", m_videoComp);
	m_encForm->addRow(obsText("OpenAVX.Output.SpeedHQQuality") + ":", m_qualityRowWidget);
	m_encForm->addRow(obsText("OpenAVX.Output.Bitrate") + ":", m_bitrateKbps);
	m_encForm->addRow(obsText("OpenAVX.Output.KeyframeInterval") + ":", m_keyframeInterval);
	m_encForm->addRow(obsText("OpenAVX.Output.ColorFormat") + ":", m_colorFormat);
	m_encForm->addRow(obsText("OpenAVX.Output.AudioCompression") + ":", m_audioComp);

	m_videoComp->setToolTip(obsText("OpenAVX.Output.Tip.VideoCompression"));
	m_speedhqQuality->setToolTip(obsText("OpenAVX.Output.Tip.SpeedHQQuality"));
	m_bitrateKbps->setToolTip(obsText("OpenAVX.Output.Tip.Bitrate"));
	m_keyframeInterval->setToolTip(obsText("OpenAVX.Output.Tip.KeyframeInterval"));
	m_colorFormat->setToolTip(obsText("OpenAVX.Output.Tip.ColorFormat"));
	m_audioComp->setToolTip(obsText("OpenAVX.Output.Tip.AudioCompression"));

	encodingLayout->addWidget(encGroup);
	encodingLayout->addStretch(1);

	tabs->addTab(encodingTab, obsText("OpenAVX.Dialog.Tab.Encoding"));

	/* === Tab 3: Servers & Network === */
	auto *netTab = new QWidget(tabs);
	auto *netTabLayout = new QVBoxLayout(netTab);
	netTabLayout->setSpacing(10);

	auto *serversGroup = new QGroupBox(obsText("OpenAVX.Dialog.ServersGroup"), netTab);
	auto *serversLayout = new QVBoxLayout(serversGroup);
	serversLayout->setSpacing(6);
	auto *serversForm = new QFormLayout();
	serversForm->setVerticalSpacing(6);

	auto *webRow = new QHBoxLayout();
	m_webPreviewEnabled = new QCheckBox(obsText("OpenAVX.Dialog.WebPreview"), serversGroup);
	m_webPreviewEnabled->setToolTip(obsText("OpenAVX.Output.Tip.WebPreview"));
	m_webPreviewPort = new QSpinBox(serversGroup);
	m_webPreviewPort->setRange(1024, 65535);
	m_webPreviewPort->setPrefix(QStringLiteral(":"));
	webRow->addWidget(m_webPreviewEnabled, 1);
	webRow->addWidget(m_webPreviewPort);

	auto *webUrlRow = new QHBoxLayout();
	m_webUrlLabel = new QLabel(serversGroup);
	m_webUrlLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
	m_webUrlLabel->setStyleSheet(QStringLiteral("color: #9aa0a6; font-size: 11px;"));
	m_copyWebBtn = new QPushButton(obsText("OpenAVX.Dialog.CopyUrl"), serversGroup);
	m_copyWebBtn->setFixedWidth(72);
	m_copyWebBtn->setEnabled(false);
	webUrlRow->addWidget(m_webUrlLabel, 1);
	webUrlRow->addWidget(m_copyWebBtn);

	auto *rtspRow = new QHBoxLayout();
	m_rtspEnabled = new QCheckBox(obsText("OpenAVX.Dialog.RtspServer"), serversGroup);
	m_rtspEnabled->setToolTip(obsText("OpenAVX.Output.Tip.RtspServer"));
	m_rtspPort = new QSpinBox(serversGroup);
	m_rtspPort->setRange(1024, 65535);
	m_rtspPort->setPrefix(QStringLiteral(":"));
	rtspRow->addWidget(m_rtspEnabled, 1);
	rtspRow->addWidget(m_rtspPort);

	auto *rtspUrlRow = new QHBoxLayout();
	m_rtspUrlLabel = new QLabel(serversGroup);
	m_rtspUrlLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
	m_rtspUrlLabel->setStyleSheet(QStringLiteral("color: #9aa0a6; font-size: 11px;"));
	m_copyRtspBtn = new QPushButton(obsText("OpenAVX.Dialog.CopyUrl"), serversGroup);
	m_copyRtspBtn->setFixedWidth(72);
	m_copyRtspBtn->setEnabled(false);
	rtspUrlRow->addWidget(m_rtspUrlLabel, 1);
	rtspUrlRow->addWidget(m_copyRtspBtn);

	serversForm->addRow(QString(), webRow);
	serversForm->addRow(QString(), webUrlRow);
	serversForm->addRow(QString(), rtspRow);
	serversForm->addRow(QString(), rtspUrlRow);
	serversLayout->addLayout(serversForm);

	m_serversHint = new QLabel(obsText("OpenAVX.Dialog.ServersMainHint"), serversGroup);
	m_serversHint->setWordWrap(true);
	m_serversHint->setStyleSheet(QStringLiteral("color: #f0ad4e; font-size: 11px;"));
	m_serversHint->hide();
	serversLayout->addWidget(m_serversHint);
	netTabLayout->addWidget(serversGroup);

	auto *netGroup = new QGroupBox(obsText("OpenAVX.Dialog.NetworkGroup"), netTab);
	auto *netForm = new QFormLayout(netGroup);
	netForm->setVerticalSpacing(8);

	m_mainGroups = new QLineEdit(netGroup);
	m_mainGroups->setMaxLength(250);
	m_mainGroups->setPlaceholderText(obsText("OpenAVX.Dialog.GroupsPlaceholder"));
	m_mainGroups->setToolTip(obsText("OpenAVX.Output.Tip.Groups"));

	m_previewGroups = new QLineEdit(netGroup);
	m_previewGroups->setMaxLength(250);
	m_previewGroups->setPlaceholderText(obsText("OpenAVX.Dialog.GroupsPlaceholder"));
	m_previewGroups->setToolTip(obsText("OpenAVX.Output.Tip.Groups"));

	m_transport = new QComboBox(netGroup);
	m_transport->addItem(obsText("OpenAVX.Output.Transport.TCP"), OAVX_TRANSPORT_TCP);
	m_transport->addItem(obsText("OpenAVX.Output.Transport.RUDP"), OAVX_TRANSPORT_RUDP);
	m_transport->addItem(obsText("OpenAVX.Output.Transport.UDP"), OAVX_TRANSPORT_UDP);
	m_transport->setToolTip(obsText("OpenAVX.Source.Tip.Transport"));

	m_bindIp = new QLineEdit(netGroup);
	m_bindIp->setMaxLength(120);
	m_bindIp->setPlaceholderText(obsText("OpenAVX.Output.BindIPPlaceholder"));
	m_bindIp->setToolTip(obsText("OpenAVX.Output.Tip.BindIP"));

	m_failoverTarget = new QLineEdit(netGroup);
	m_failoverTarget->setMaxLength(250);
	m_failoverTarget->setPlaceholderText(obsText("OpenAVX.Output.FailoverTargetPlaceholder"));
	m_failoverTarget->setToolTip(obsText("OpenAVX.Output.Tip.FailoverTarget"));

	netForm->addRow(obsText("OpenAVX.Dialog.MainGroups") + ":", m_mainGroups);
	netForm->addRow(obsText("OpenAVX.Dialog.PreviewGroups") + ":", m_previewGroups);
	netForm->addRow(obsText("OpenAVX.Output.Transport") + ":", m_transport);
	netForm->addRow(obsText("OpenAVX.Output.BindIP") + ":", m_bindIp);
	netForm->addRow(obsText("OpenAVX.Output.FailoverTarget") + ":", m_failoverTarget);
	netTabLayout->addWidget(netGroup);
	netTabLayout->addStretch(1);

	tabs->addTab(netTab, obsText("OpenAVX.Dialog.Tab.Network"));

	root->addWidget(tabs, 1);

	m_errorBanner = new QLabel(this);
	m_errorBanner->setWordWrap(true);
	m_errorBanner->setStyleSheet(QStringLiteral("color: #d9534f; font-weight: bold;"));
	m_errorBanner->hide();
	root->addWidget(m_errorBanner);

	m_buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Apply | QDialogButtonBox::Cancel |
						   QDialogButtonBox::RestoreDefaults,
					   this);
	root->addWidget(m_buttonBox);

	connect(m_videoComp, &QComboBox::currentIndexChanged, this, &OpenAVXOutputDialog::onVideoCompressionChanged);
	connect(m_speedhqQuality, &QSlider::valueChanged, this, &OpenAVXOutputDialog::onQualityChanged);
	connect(m_mainEnabled, &QCheckBox::toggled, this, [this](bool) { updateServersHint(); });
	connect(m_webPreviewEnabled, &QCheckBox::toggled, this, [this](bool) { updateServersHint(); });
	connect(m_rtspEnabled, &QCheckBox::toggled, this, [this](bool) { updateServersHint(); });
	connect(m_copyWebBtn, &QPushButton::clicked, this, [this]() {
		if (!m_webUrlLabel->text().isEmpty()) {
			QApplication::clipboard()->setText(m_webUrlLabel->text());
			showBanner(obsText("OpenAVX.Dialog.CopiedUrl"), false);
		}
	});
	connect(m_copyRtspBtn, &QPushButton::clicked, this, [this]() {
		if (!m_rtspUrlLabel->text().isEmpty()) {
			QApplication::clipboard()->setText(m_rtspUrlLabel->text());
			showBanner(obsText("OpenAVX.Dialog.CopiedUrl"), false);
		}
	});
	connect(m_buttonBox, &QDialogButtonBox::accepted, this, &OpenAVXOutputDialog::onAccept);
	connect(m_buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
	if (QPushButton *applyBtn = m_buttonBox->button(QDialogButtonBox::Apply))
		connect(applyBtn, &QPushButton::clicked, this, &OpenAVXOutputDialog::onApply);
	if (QPushButton *defaultsBtn = m_buttonBox->button(QDialogButtonBox::RestoreDefaults))
		connect(defaultsBtn, &QPushButton::clicked, this, &OpenAVXOutputDialog::onRestoreDefaults);

	reloadFromController();

	m_statusTimer = new QTimer(this);
	m_statusTimer->setInterval(500);
	connect(m_statusTimer, &QTimer::timeout, this, &OpenAVXOutputDialog::refreshStatus);
	m_statusTimer->start();
}

void OpenAVXOutputDialog::reloadFromController()
{
	struct openavx_output_config cfg;
	openavx_output_controller_get_config(&cfg);
	loadConfigToUi(cfg);
	m_errorBanner->hide();
	refreshStatus();
}

void OpenAVXOutputDialog::loadConfigToUi(const struct openavx_output_config &cfg)
{
	m_mainEnabled->setChecked(cfg.main_enabled);
	m_mainName->setText(QString::fromUtf8(cfg.main_name));
	m_mainGroups->setText(QString::fromUtf8(cfg.main_groups));
	m_webPreviewEnabled->setChecked(cfg.web_preview_enabled);
	m_webPreviewPort->setValue(cfg.web_preview_port >= 1024 ? cfg.web_preview_port : 8080);
	m_rtspEnabled->setChecked(cfg.rtsp_enabled);
	m_rtspPort->setValue(cfg.rtsp_port >= 1024 ? cfg.rtsp_port : 8554);

	m_previewEnabled->setChecked(cfg.preview_enabled);
	m_previewName->setText(QString::fromUtf8(cfg.preview_name));
	m_previewGroups->setText(QString::fromUtf8(cfg.preview_groups));
	m_previewIncludeAudio->setChecked(cfg.preview_include_audio);

	selectComboData(m_videoComp, cfg.video_compression);
	m_speedhqQuality->setValue(cfg.speedhq_quality);
	m_speedhqQualityVal->setText(QString::number(cfg.speedhq_quality));
	m_bitrateKbps->setValue(cfg.bitrate_kbps > 0 ? cfg.bitrate_kbps : 6000);
	m_keyframeInterval->setValue(cfg.keyframe_interval > 0 ? cfg.keyframe_interval : 60);
	m_passphrase->setText(QString::fromUtf8(cfg.passphrase));
	m_bindIp->setText(QString::fromUtf8(cfg.bind_ip));
	m_failoverTarget->setText(QString::fromUtf8(cfg.failover_target));
	selectComboData(m_colorFormat, cfg.color_format);
	selectComboData(m_audioComp, cfg.audio_compression);
	selectComboData(m_transport, cfg.transport);

	onVideoCompressionChanged(m_videoComp->currentIndex());
	updateServersHint();
}

struct openavx_output_config OpenAVXOutputDialog::readConfigFromUi() const
{
	struct openavx_output_config cfg;
	openavx_output_config_set_defaults(&cfg);

	cfg.main_enabled = m_mainEnabled->isChecked();
	copyQStringToBuf(m_mainName->text(), cfg.main_name, sizeof(cfg.main_name));
	copyQStringToBuf(m_mainGroups->text(), cfg.main_groups, sizeof(cfg.main_groups));
	cfg.web_preview_enabled = m_webPreviewEnabled->isChecked();
	cfg.web_preview_port = m_webPreviewPort->value();
	cfg.rtsp_enabled = m_rtspEnabled->isChecked();
	cfg.rtsp_port = m_rtspPort->value();

	cfg.preview_enabled = m_previewEnabled->isChecked();
	copyQStringToBuf(m_previewName->text(), cfg.preview_name, sizeof(cfg.preview_name));
	copyQStringToBuf(m_previewGroups->text(), cfg.preview_groups, sizeof(cfg.preview_groups));
	cfg.preview_include_audio = m_previewIncludeAudio->isChecked();

	cfg.video_compression = m_videoComp->currentData().toInt();
	cfg.speedhq_quality = m_speedhqQuality->value();
	cfg.bitrate_kbps = m_bitrateKbps->value();
	cfg.keyframe_interval = m_keyframeInterval->value();
	copyQStringToBuf(m_passphrase->text(), cfg.passphrase, sizeof(cfg.passphrase));
	copyQStringToBuf(m_bindIp->text(), cfg.bind_ip, sizeof(cfg.bind_ip));
	copyQStringToBuf(m_failoverTarget->text(), cfg.failover_target, sizeof(cfg.failover_target));
	cfg.color_format = m_colorFormat->currentData().toInt();
	cfg.audio_compression = m_audioComp->currentData().toInt();
	cfg.transport = m_transport->currentData().toInt();
	return cfg;
}

bool OpenAVXOutputDialog::validateUi(QString &errorOut) const
{
	if (m_mainEnabled->isChecked() && m_previewEnabled->isChecked()) {
		QString mainName = m_mainName->text().trimmed();
		QString prevName = m_previewName->text().trimmed();
		if (mainName.isEmpty())
			mainName = QStringLiteral("OpenAVX Program Output");
		if (prevName.isEmpty())
			prevName = QStringLiteral("OpenAVX Preview Output");
		if (mainName == prevName) {
			errorOut = obsText("OpenAVX.Dialog.DuplicateNameError");
			return false;
		}
	}
	return true;
}

void OpenAVXOutputDialog::showBanner(const QString &msg, bool isError)
{
	m_errorBanner->setStyleSheet(isError ? QStringLiteral("color: #d9534f; font-weight: bold;")
					     : QStringLiteral("color: #5cb85c; font-weight: bold;"));
	m_errorBanner->setText(msg);
	m_errorBanner->show();
}

void OpenAVXOutputDialog::updateServersHint()
{
	if (!m_serversHint)
		return;
	bool needMain = !m_mainEnabled->isChecked() && (m_webPreviewEnabled->isChecked() || m_rtspEnabled->isChecked());
	m_serversHint->setVisible(needMain);
}

bool OpenAVXOutputDialog::applyCurrentConfig()
{
	QString validationErr;
	if (!validateUi(validationErr)) {
		showBanner(validationErr, true);
		return false;
	}

	struct openavx_output_config cfg = readConfigFromUi();
	bool ok = openavx_output_controller_apply_config(&cfg, false);

	/* Reflect sanitized values back into inputs cleanly */
	struct openavx_output_config applied;
	openavx_output_controller_get_config(&applied);
	loadConfigToUi(applied);
	refreshStatus();

	if (!ok) {
		struct openavx_output_status st;
		openavx_output_controller_get_status(&st);
		QString msg;
		if (st.main_last_error[0])
			msg = QString::fromUtf8(st.main_last_error);
		else if (st.preview_last_error[0])
			msg = QString::fromUtf8(st.preview_last_error);
		else
			msg = obsText("OpenAVX.Dialog.StartFailed");
		showBanner(msg, true);
		return false;
	}

	m_errorBanner->hide();
	return true;
}

void OpenAVXOutputDialog::onVideoCompressionChanged(int index)
{
	Q_UNUSED(index);
	int vc = m_videoComp->currentData().toInt();
	bool isSpeedhq = (vc == 1);
	bool isHw = (vc == 2 || vc == 3 || vc == 4);
	if (m_encForm) {
		m_encForm->setRowVisible(m_qualityRowWidget, isSpeedhq);
		m_encForm->setRowVisible(m_bitrateKbps, isHw);
		m_encForm->setRowVisible(m_keyframeInterval, isHw);
	}
}

void OpenAVXOutputDialog::onQualityChanged(int value)
{
	m_speedhqQualityVal->setText(QString::number(value));
}

void OpenAVXOutputDialog::onApply()
{
	if (applyCurrentConfig())
		showBanner(obsText("OpenAVX.Dialog.SettingsApplied"), false);
}

void OpenAVXOutputDialog::onAccept()
{
	if (applyCurrentConfig())
		accept();
}

void OpenAVXOutputDialog::onRestoreDefaults()
{
	struct openavx_output_config defCfg;
	openavx_output_config_set_defaults(&defCfg);
	loadConfigToUi(defCfg);
	m_errorBanner->hide();
}

void OpenAVXOutputDialog::refreshStatus()
{
	struct openavx_output_status st;
	openavx_output_controller_get_status(&st);

	auto formatStatusText = [](bool active, uint64_t bytes, int frames, int dropped, float congestion,
				   bool tally_pgm, bool tally_pvw, int clients, const char *lastErr) -> QString {
		if (active) {
			QString s = QString("%1 — %2 | %3 %4 | %5 %6 | %7 %8")
					    .arg(obsText("OpenAVX.Dialog.StatusActive"))
					    .arg(formatByteCount(bytes))
					    .arg(frames)
					    .arg(obsText("OpenAVX.Dialog.StatusFrames"))
					    .arg(dropped)
					    .arg(obsText("OpenAVX.Dialog.StatusDropped"))
					    .arg(clients)
					    .arg(obsText("OpenAVX.Dialog.StatusClients"));
			if (congestion >= 0.5f)
				s += QStringLiteral(" (") + obsText("OpenAVX.Dialog.StatusCongested") +
				     QStringLiteral(")");
			if (tally_pgm && tally_pvw)
				s += QStringLiteral(" | ") + obsText("OpenAVX.Dialog.StatusTallyProgram") +
				     QStringLiteral(" ") + obsText("OpenAVX.Dialog.StatusTallyPreview");
			else if (tally_pgm)
				s += QStringLiteral(" | ") + obsText("OpenAVX.Dialog.StatusTallyProgram");
			else if (tally_pvw)
				s += QStringLiteral(" | ") + obsText("OpenAVX.Dialog.StatusTallyPreview");
			return s;
		}
		if (lastErr && lastErr[0]) {
			return obsText("OpenAVX.Dialog.StatusError") + QStringLiteral(": ") +
			       QString::fromUtf8(lastErr);
		}
		return obsText("OpenAVX.Dialog.StatusStopped");
	};

	auto statusStyle = [](bool active, const char *lastErr) -> QString {
		if (active)
			return QStringLiteral("color: #5cb85c; font-weight: 600;");
		if (lastErr && lastErr[0])
			return QStringLiteral("color: #d9534f; font-weight: 600;");
		return QStringLiteral("color: #9aa0a6;");
	};

	QString mainText = formatStatusText(st.main_active, st.main_total_bytes, st.main_total_frames,
					    st.main_dropped_frames, st.main_congestion, st.main_tally_program,
					    st.main_tally_preview, st.main_clients, st.main_last_error);
	if (st.main_active) {
		if (st.main_web_url[0])
			mainText += QStringLiteral(" | Web: ") + QString::fromUtf8(st.main_web_url);
		if (st.main_rtsp_port > 0)
			mainText += QStringLiteral(" | RTSP: :%1").arg(st.main_rtsp_port);
	}
	m_mainStatus->setStyleSheet(statusStyle(st.main_active, st.main_last_error));
	m_mainStatus->setText(mainText);

	m_previewStatus->setStyleSheet(statusStyle(st.preview_active, st.preview_last_error));
	m_previewStatus->setText(formatStatusText(st.preview_active, st.preview_total_bytes, st.preview_total_frames,
						  st.preview_dropped_frames, st.preview_congestion,
						  st.preview_tally_program, st.preview_tally_preview,
						  st.preview_clients, st.preview_last_error));

	if (st.main_active && st.main_web_url[0]) {
		m_webUrlLabel->setStyleSheet(QStringLiteral("color: #5cb85c; font-size: 11px;"));
		m_webUrlLabel->setText(QString::fromUtf8(st.main_web_url));
		m_copyWebBtn->setEnabled(true);
	} else {
		m_webUrlLabel->setStyleSheet(QStringLiteral("color: #9aa0a6; font-size: 11px;"));
		m_webUrlLabel->setText(obsText("OpenAVX.Dialog.ServerInactiveUrl"));
		m_copyWebBtn->setEnabled(false);
	}

	if (st.main_active && st.main_rtsp_port > 0) {
		m_rtspUrlLabel->setStyleSheet(QStringLiteral("color: #5cb85c; font-size: 11px;"));
		m_rtspUrlLabel->setText(QStringLiteral("rtsp://127.0.0.1:%1/live").arg(st.main_rtsp_port));
		m_copyRtspBtn->setEnabled(true);
	} else {
		m_rtspUrlLabel->setStyleSheet(QStringLiteral("color: #9aa0a6; font-size: 11px;"));
		m_rtspUrlLabel->setText(obsText("OpenAVX.Dialog.ServerInactiveUrl"));
		m_copyRtspBtn->setEnabled(false);
	}
}

extern "C" void openavx_output_ui_init(void)
{
	if (!g_ui_exit_hooked) {
		obs_frontend_add_event_callback(onFrontendUiEvent, nullptr);
		g_ui_exit_hooked = true;
	}
	obs_frontend_add_tools_menu_item(obs_module_text("OpenAVX.ToolsMenu.OutputSettings"), onToolsMenuClicked,
					 nullptr);
}

extern "C" void openavx_output_ui_shutdown(void)
{
	if (g_ui_exit_hooked) {
		obs_frontend_remove_event_callback(onFrontendUiEvent, nullptr);
		g_ui_exit_hooked = false;
	}
	closeDialogOnUi();
}
