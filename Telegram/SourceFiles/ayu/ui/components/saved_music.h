// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#pragma once

#include "ui/widgets/buttons.h"

#include <atomic>

namespace Ui {
class FlatLabel;
} // namespace Ui

namespace Data {
class DocumentMedia;
}

namespace Info::Profile {

struct MusicButtonData;

struct ResultCover {
	QPixmap pix;
	QColor bg;
	bool noCover = true;
};

class AyuMusicButton final : public Ui::RippleButton {
public:
	AyuMusicButton(
		QWidget *parent,
		MusicButtonData data,
		std::optional<QColor> overrideBg,
		Fn<void()> handler);
	~AyuMusicButton();

	void updateData(MusicButtonData data);
	void setOverrideBg(std::optional<QColor> color);
	void setCoverActive(bool active);
	void refreshColors();

	rpl::producer<> onReady() const {
		return _onReady.events();
	}

private:
	void downloadAndMakeCover(FullMsgId msgId);
	void makeCover();
	void prepareCover(QImage image);
	void applyCover(QImage image, std::optional<QRgb> color);
	void cancelCover();

	void paintEvent(QPaintEvent *e) override;
	int resizeGetHeight(int newWidth) override;

	std::unique_ptr<Ui::FlatLabel> _performer;
	std::unique_ptr<Ui::FlatLabel> _title;
	std::shared_ptr<Data::DocumentMedia> _mediaView;
	std::optional<ResultCover> _currentCover;
	rpl::event_stream<> _onReady;
	rpl::lifetime _coverLifetime;
	rpl::lifetime _downloadLifetime;
	std::shared_ptr<std::atomic_bool> _coverCancelled;
	std::optional<QColor> _adaptiveColor;
	FullMsgId _msgId;
	uint64 _coverGeneration = 0;
	bool _coverActive = false;

	QString _performerText;
	QString _titleText;

	std::optional<QColor> _overrideBg;

};

} // namespace Info::Profile
