// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "ayu/ui/components/saved_music.h"
#include "crl/crl_async.h"
#include "crl/crl_on_main.h"

#include "ayu/ayu_settings.h"
#include "ayu/ui/utils/color_utils.h"
#include "ayu/ui/utils/itunes_search.h"
#include "ayu/ui/utils/palette.h"
#include "data/data_document.h"
#include "data/data_document_media.h"
#include "data/data_file_origin.h"
#include "data/data_session.h"
#include "info/profile/info_profile_music_button.h"
#include "main/main_session.h"
#include "main/main_session_settings.h"
#include "ui/painter.h"
#include "ui/image/image.h"
#include "ui/ui_utility.h"
#include "ui/widgets/labels.h"
#include "window/themes/window_theme.h"

#include <QSvgRenderer>

#include "styles/palette.h"
#include "styles/style_info.h"

namespace Info::Profile {

namespace {

QColor performerColor(255, 255, 255, 153); // white 60%

QRgb AdjustHsl(QRgb color, float luminance, float saturation = -1.0f) {
	auto hsl = Ayu::Ui::ColorUtils::colorToHSL(color);

	if (saturation > 0.0f) {
		hsl[1] = std::min(hsl[1] * saturation, 1.0f);
	}

	hsl[2] = std::min(hsl[2] * luminance, 1.0f);

	return Ayu::Ui::ColorUtils::HSLToRGB(hsl);
}

QRgb BlendARGB(QRgb color1, QRgb color2, float ratio) {
	const auto inverseRatio = 1.0f - ratio;
	const auto r = static_cast<int>(qRed(color1) * inverseRatio + qRed(color2) * ratio);
	const auto g = static_cast<int>(qGreen(color1) * inverseRatio + qGreen(color2) * ratio);
	const auto b = static_cast<int>(qBlue(color1) * inverseRatio + qBlue(color2) * ratio);
	const auto a = static_cast<int>(qAlpha(color1) * inverseRatio + qAlpha(color2) * ratio);
	return qRgba(r, g, b, a);
}

QColor GetNoCoverBgColor(std::optional<QColor> overrideBg) {
	if (overrideBg) {
		return Ui::BlendColors(
			*overrideBg,
			Qt::black,
			st::infoProfileTopBarActionButtonBgOpacity);
	}

	return st::shadowFg->c;
}

QPixmap MakeNoCoverImage(const QSize &size) {
	static QPixmap result;
	static auto resultTheme = Window::Theme::Background()->id();
	if (!result.isNull() && result.size() == size && resultTheme == Window::Theme::Background()->id()) {
		return result;
	}
	resultTheme = Window::Theme::Background()->id();

	auto image = QImage(size, QImage::Format_ARGB32);
	{
		auto p = Painter(&image);
		auto hq = PainterHighQualityEnabler(p);

		const auto bgColor = Window::Theme::IsNightMode()
								 ? st::windowBoldFg->c.darker()
								 : st::windowBoldFg->c.lighter();
		image.fill(bgColor);

		auto svgIcon = QSvgRenderer(u":/gui/icons/ayu/nocover.svg"_q);
		p.setPen(st::windowBoldFg->p);
		svgIcon.render(&p, QRect(0, 0, size.width(), size.height()));
	}
	const auto img = Image(std::move(image));
	result = img.pix(size, Images::PrepareArgs{.options = Images::Option::RoundSmall});
	return result;
}

} // namespace

std::optional<QRgb> ExtractColorFromCover(const QImage &cover) {
	const auto palette = Ayu::Ui::Palette::from(cover).generate();

	const auto *swatch = palette.darkVibrantSwatch();
	if (!swatch) {
		swatch = palette.mutedSwatch();
	}
	if (!swatch) {
		swatch = palette.darkMutedSwatch();
	}
	if (!swatch) {
		swatch = palette.dominantSwatch();
	}

	if (!swatch) {
		return std::nullopt;
	}

	const auto extractedColor = swatch->rgb();

	constexpr auto whiteColor = qRgb(255, 255, 255);
	const auto contrast = Ayu::Ui::ColorUtils::calculateContrast(whiteColor, extractedColor);

	auto adjustedColor = extractedColor;
	if (contrast > 15.0f) {
		adjustedColor = AdjustHsl(extractedColor, 2.0f);
	} else if (contrast < 10.0f) {
		adjustedColor = AdjustHsl(extractedColor, 0.5f);
	}

	if (Ayu::Ui::ColorUtils::calculateContrast(whiteColor, adjustedColor) < 3.0f) {
		adjustedColor = BlendARGB(adjustedColor, qRgb(0, 0, 0), 0.3f);
	}

	return adjustedColor;
}

AyuMusicButton::AyuMusicButton(
	QWidget *parent,
	MusicButtonData data,
	std::optional<QColor> overrideBg,
	Fn<void()> handler)
	: RippleButton(parent, st::infoMusicButtonRipple)
	  , _performer(std::make_unique<Ui::FlatLabel>(
		  this,
		  data.performer,
		  st::infoMusicButtonPerformer))
	  , _title(std::make_unique<Ui::FlatLabel>(
		  this,
		  data.title,
		  st::infoMusicButtonTitle))
	  , _mediaView(data.mediaView)
	  , _overrideBg(overrideBg) {
	_performerText = data.performer;
	_titleText = data.title;
	rpl::combine(
		_title->naturalWidthValue(),
		_performer->naturalWidthValue()
	) | rpl::on_next([=] {
		resizeToWidth(widthNoMargins());
	}, lifetime());

	_title->setAttribute(Qt::WA_TransparentForMouseEvents);
	_performer->setAttribute(Qt::WA_TransparentForMouseEvents);

	_msgId = data.msgId;

	setClickedCallback(std::move(handler));
}

AyuMusicButton::~AyuMusicButton() {
	cancelCover();
}

void AyuMusicButton::updateData(MusicButtonData data) {
	const auto changed = (_msgId != data.msgId)
		|| (_performerText != data.performer)
		|| (_titleText != data.title)
		|| (_mediaView ? _mediaView->owner().get() : nullptr)
			!= (data.mediaView ? data.mediaView->owner().get() : nullptr);
	if (!changed) {
		refreshColors();
		return;
	}
	_performer->setText(data.performer);
	_title->setText(data.title);
	_performerText = data.performer;
	_titleText = data.title;
	_mediaView = data.mediaView;
	_msgId = data.msgId;
	cancelCover();
	_currentCover.reset();
	_adaptiveColor.reset();
	if (_coverActive) {
		downloadAndMakeCover(_msgId);
	}
	refreshColors();
	resizeToWidth(widthNoMargins());
}

void AyuMusicButton::setOverrideBg(std::optional<QColor> color) {
	_overrideBg = color;
	refreshColors();
}

void AyuMusicButton::setCoverActive(bool active) {
	if (_coverActive == active) {
		return;
	}
	_coverActive = active;
	if (!active) {
		cancelCover();
	} else if (!_currentCover) {
		downloadAndMakeCover(_msgId);
	}
}

void AyuMusicButton::cancelCover() {
	++_coverGeneration;
	_downloadLifetime.destroy();
	_coverLifetime.destroy();
	if (const auto cancelled = base::take(_coverCancelled)) {
		cancelled->store(true);
	}
}

void AyuMusicButton::downloadAndMakeCover(FullMsgId msgId) {
	if (_mediaView
		&& _mediaView->owner()->isSongWithCover()
		&& !_mediaView->thumbnail()) {
		const auto settings = &_mediaView->owner()->session()
			.settings().autoDownload();
		if (settings->shouldDownload(
				Data::AutoDownload::Source::User,
				Data::AutoDownload::Type::File,
				_mediaView->owner()->size)) {
			_mediaView->thumbnailWanted(Data::FileOrigin(msgId));
			if (_mediaView->thumbnail()) {
				makeCover();
				return;
			}
			const auto generation = _coverGeneration;
			_mediaView->owner()->owner().session().downloaderTaskFinished(
			) | rpl::filter([=] {
				return _mediaView && _mediaView->thumbnail();
			}) | rpl::take(1) | rpl::on_next([=] {
				crl::on_main(this, [=] {
					if (_coverActive && generation == _coverGeneration) {
						makeCover();
					}
				});
			}, _downloadLifetime);
			return;
		}
	}
	makeCover();
}

void AyuMusicButton::makeCover() {
	if (!_coverActive) {
		return;
	}
	if (_mediaView) {
		if (const auto thumbnail = _mediaView->thumbnail()) {
			prepareCover(thumbnail->original());
			return;
		}
	}
	const auto generation = _coverGeneration;
	Ayu::Ui::Itunes::FetchCover(
		_performerText,
		_titleText
	) | rpl::on_next([=](QImage image) {
		if (_coverActive && generation == _coverGeneration) {
			prepareCover(std::move(image));
		}
	}, _coverLifetime);
}

void AyuMusicButton::prepareCover(QImage image) {
	if (image.isNull()) {
		applyCover({}, std::nullopt);
		return;
	}
	const auto generation = _coverGeneration;
	const auto cancelled = std::make_shared<std::atomic_bool>(false);
	_coverCancelled = cancelled;
	const auto weak = base::make_weak(this);
	crl::async([=, image = std::move(image)]() mutable {
		if (cancelled->load()) {
			return;
		}
		const auto color = ExtractColorFromCover(image);
		if (cancelled->load()) {
			return;
		}
		crl::on_main([=, image = std::move(image)]() mutable {
			const auto strong = weak.get();
			if (strong
				&& !cancelled->load()
				&& strong->_coverActive
				&& strong->_coverGeneration == generation) {
				strong->applyCover(std::move(image), color);
			}
		});
	});
}

void AyuMusicButton::applyCover(QImage image, std::optional<QRgb> color) {
	const auto &font = st::infoMusicButtonTitle.style.font;
	const auto skip = st::normalFont->spacew / 2;
	const auto size = QSize(
		font->height + skip + font->height,
		font->height + skip + font->height);
	const auto noCover = image.isNull();
	auto pix = QPixmap();
	if (noCover) {
		pix = MakeNoCoverImage(size);
	} else {
		const auto prepared = Image(std::move(image));
		pix = prepared.pix(
			size,
			Images::PrepareArgs{
				.options = Images::Option::RoundSmall,
				.outer = size,
			});
	}
	_adaptiveColor = color ? std::make_optional(QColor::fromRgb(*color))
		: std::nullopt;
	_currentCover = {
		.pix = std::move(pix),
		.bg = GetNoCoverBgColor(_overrideBg),
		.noCover = noCover,
	};
	refreshColors();
	_onReady.fire({});
}

void AyuMusicButton::refreshColors() {
	if (!_currentCover) {
		return;
	}
	const auto adaptive = AyuSettings::getInstance().adaptiveCoverColor()
		&& !_currentCover->noCover
		&& _adaptiveColor.has_value();
	_currentCover->bg = adaptive
		? *_adaptiveColor
		: GetNoCoverBgColor(_overrideBg);
	_title->setTextColorOverride(adaptive
		? Qt::white
		: _overrideBg ? st::groupCallMembersFg->c : st::windowBoldFg->c);
	_performer->setTextColorOverride(adaptive
		? performerColor
		: _overrideBg ? st::groupCallMembersFg->c : st::windowBoldFg->c);
	update();
	_title->update();
	_performer->update();
}

void AyuMusicButton::paintEvent(QPaintEvent *e) {
	if (!_currentCover) {
		return;
	}

	auto p = Painter(this);

	const auto &font = st::infoMusicButtonTitle.style.font;
	const auto skip = st::normalFont->spacew / 2;
	const auto size = font->height + skip + font->height;

	const auto &settings = AyuSettings::getInstance();
	const auto &cover = *_currentCover;
	if (cover.noCover || !settings.adaptiveCoverColor() || !_adaptiveColor) {
		p.fillRect(e->rect(), cover.bg);
		paintRipple(p, QPoint());
	} else {
		QRadialGradient gradient(rect().topRight(), rect().width() * 2.0);
		gradient.setColorAt(0, cover.bg);
		gradient.setColorAt(1, QColor::fromRgb(AdjustHsl(cover.bg.rgb(), 1.5f)));
		p.fillRect(rect(), gradient);
	}

	if (!cover.pix.isNull()) {
		auto hq = PainterHighQualityEnabler(p);
		const auto coverRect = QRect(st::infoMusicButtonPadding.left(), st::infoMusicButtonPadding.top(), size, size);
		p.drawPixmap(coverRect, cover.pix);
	}
}

int AyuMusicButton::resizeGetHeight(int newWidth) {
	const auto padding = st::infoMusicButtonPadding;
	const auto &font = st::infoMusicButtonTitle.style.font;

	const auto top = padding.top();
	const auto skip = st::normalFont->spacew / 2;

	const auto coverSize = font->height + skip + font->height;

	const auto available = newWidth - padding.left() - padding.right() - coverSize;
	_title->resizeToNaturalWidth(available);
	_title->moveToLeft(st::infoMusicButtonPadding.left() + padding.left() + coverSize, top);
	_performer->resizeToNaturalWidth(available);
	_performer->moveToLeft(st::infoMusicButtonPadding.left() + padding.left() + coverSize, top + font->height + skip);

	return padding.top() + font->height + skip + font->height + padding.bottom();
}

} // namespace Info::Profile
