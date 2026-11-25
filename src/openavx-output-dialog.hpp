/*
OpenAVX for OBS Studio - Qt Output Settings Dialog Header
Copyright (C) 2026 Saif Mukhtar <saifmukhtar20@gmail.com>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

#ifdef __cplusplus

#include <QDialog>

class QCheckBox;
class QComboBox;
class QDialogButtonBox;
class QFormLayout;
class QLabel;
class QLineEdit;
class QPushButton;
class QSlider;
class QSpinBox;
class QTimer;

struct openavx_output_config;

class OpenAVXOutputDialog : public QDialog {
	Q_OBJECT

public:
	explicit OpenAVXOutputDialog(QWidget *parent = nullptr);
	~OpenAVXOutputDialog() override = default;

	void reloadFromController();

private slots:
	void onVideoCompressionChanged(int index);
	void onQualityChanged(int value);
	void onApply();
	void onAccept();
	void onRestoreDefaults();
	void refreshStatus();

private:
	void loadConfigToUi(const struct openavx_output_config &cfg);
	struct openavx_output_config readConfigFromUi() const;
	bool validateUi(QString &errorOut) const;
	bool applyCurrentConfig();
	void showBanner(const QString &msg, bool isError);
	void updateServersHint();

	QCheckBox *m_mainEnabled = nullptr;
	QLineEdit *m_mainName = nullptr;
	QLineEdit *m_mainGroups = nullptr;
	QCheckBox *m_webPreviewEnabled = nullptr;
	QSpinBox *m_webPreviewPort = nullptr;
	QLabel *m_webUrlLabel = nullptr;
	QPushButton *m_copyWebBtn = nullptr;
	QCheckBox *m_rtspEnabled = nullptr;
	QSpinBox *m_rtspPort = nullptr;
	QLabel *m_rtspUrlLabel = nullptr;
	QPushButton *m_copyRtspBtn = nullptr;
	QLabel *m_serversHint = nullptr;
	QLabel *m_mainStatus = nullptr;

	QCheckBox *m_previewEnabled = nullptr;
	QLineEdit *m_previewName = nullptr;
	QLineEdit *m_previewGroups = nullptr;
	QCheckBox *m_previewIncludeAudio = nullptr;
	QLabel *m_previewStatus = nullptr;

	QComboBox *m_videoComp = nullptr;
	QFormLayout *m_encForm = nullptr;
	QWidget *m_qualityRowWidget = nullptr;
	QSlider *m_speedhqQuality = nullptr;
	QLabel *m_speedhqQualityVal = nullptr;
	QSpinBox *m_bitrateKbps = nullptr;
	QSpinBox *m_keyframeInterval = nullptr;
	QLineEdit *m_passphrase = nullptr;
	QLineEdit *m_bindIp = nullptr;
	QLineEdit *m_failoverTarget = nullptr;
	QComboBox *m_colorFormat = nullptr;
	QComboBox *m_audioComp = nullptr;
	QComboBox *m_transport = nullptr;

	QLabel *m_errorBanner = nullptr;
	QDialogButtonBox *m_buttonBox = nullptr;
	QTimer *m_statusTimer = nullptr;
};

extern "C" {
#endif

void openavx_output_ui_init(void);
void openavx_output_ui_shutdown(void);

#ifdef __cplusplus
}
#endif
