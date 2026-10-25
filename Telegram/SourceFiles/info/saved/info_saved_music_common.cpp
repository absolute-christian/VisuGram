/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "info/saved/info_saved_music_common.h"

#include "ayu/ayu_settings.h"
#include "ayu/ui/components/saved_music.h"
#include "ayu/utils/telegram_helpers.h"
#include "data/data_document.h"
#include "data/data_peer.h"
#include "data/data_saved_music.h"
#include "data/data_saved_sublist.h"
#include "history/history_item.h"
#include "info/info_controller.h"
#include "info/info_memento.h"
#include "info/profile/info_profile_music_button.h"
#include "info/saved/info_saved_music_widget.h"
#include "ui/text/format_song_document_name.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/popup_menu.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "ui/vertical_list.h"

#include "lang_auto.h"

#include "styles/style_menu_icons.h"

namespace Info::Saved {

namespace {

[[nodiscard]] Profile::MusicButtonData DocumentMusicButtonData(
		not_null<DocumentData*> document, not_null<HistoryItem*> message) {
	const auto name = Ui::Text::FormatSongNameFor(document);

	return {
		.name = name,
		.title = name.composedName().title,
		.performer = name.composedName().performer,
		.msgId = message->fullId(),
		.mediaView = document->createMediaView(),
	};
}

} // namespace

rpl::producer<bool> SetupSavedMusic(
		not_null<Ui::VerticalLayout*> container,
		not_null<Info::Controller*> controller,
		not_null<PeerData*> peer,
		rpl::producer<std::optional<QColor>> topBarColor) {
	struct State {
		rpl::variable<bool> hasMusic = false;
		QPointer<Ui::SlideWrap<Profile::AyuMusicButton>> button;
	};
	const auto state = container->lifetime().make_state<State>();
	auto musicValue = Data::SavedMusic::Supported(peer->id)
		? Data::SavedMusicList(
			peer,
			nullptr,
			1
		) | rpl::map([=](const Data::SavedMusicSlice &data) {
			return data.size() ? data[0].get() : nullptr;
		}) | rpl::type_erased
		: rpl::single<HistoryItem*>((HistoryItem*)(nullptr));

	const auto divider = container->add(
		object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			container,
			object_ptr<Ui::VerticalLayout>(container)));

	container->events() | rpl::on_next([=](not_null<QEvent*> event) {
		if (state->button
			&& (event->type() == QEvent::Show
				|| event->type() == QEvent::Hide)) {
			state->button->entity()->setCoverActive(
				event->type() == QEvent::Show);
		}
	}, container->lifetime());

	rpl::combine(
		std::move(musicValue),
		std::move(topBarColor)
	) | rpl::on_next([=](
			HistoryItem *item,
			std::optional<QColor> color) {
		const auto document = (item && item->media())
			? item->media()->document()
			: nullptr;
		if (!document) {
			delete state->button.data();
			divider->toggle(false, anim::type::normal);
			state->hasMusic = false;
			return;
		}
		const auto data = DocumentMusicButtonData(document, item);
		if (state->button) {
			state->button->entity()->updateData(data);
			state->button->entity()->setOverrideBg(color);
			return;
		}
		const auto musicButton = divider->entity()->add(
			object_ptr<Ui::SlideWrap<Profile::AyuMusicButton>>(
				divider->entity(),
				object_ptr<Profile::AyuMusicButton>(
					divider->entity(),
					data,
					color,
					[window = controller, peer] {
						window->showSection(Info::Saved::MakeMusic(peer));
					})));
		state->button = musicButton;
		musicButton->hide(anim::type::instant);
		musicButton->ease = anim::easeOutCubic;
		musicButton->setDuration(250);
		musicButton->entity()->setAcceptBoth(true);
		musicButton->entity()->clicks() | rpl::filter([=](
				Qt::MouseButton mouseButton) {
			return mouseButton == Qt::RightButton;
		}) | rpl::on_next([=] {
			const auto &settings = AyuSettings::getInstance();
			const auto contextMenu = new Ui::PopupMenu(
				musicButton,
				st::popupMenuWithIcons);
			contextMenu->setAttribute(Qt::WA_DeleteOnClose);
			const auto weak = base::make_weak(musicButton);
			contextMenu->addAction(
				settings.adaptiveCoverColor()
					? tr::ayu_DisableColorfulCover(tr::now)
					: tr::ayu_EnableColorfulCover(tr::now),
				[=] {
					const auto strong = weak.get();
					if (!strong) {
						return;
					}
					auto &settings = AyuSettings::getInstance();
					settings.setAdaptiveCoverColor(
						!settings.adaptiveCoverColor());
					strong->entity()->refreshColors();
				},
				&st::menuIconPalette);
			contextMenu->popup(QCursor::pos());
		}, musicButton->lifetime());

		const auto weak = base::make_weak(musicButton);
		musicButton->entity()->onReady() | rpl::on_next([=] {
			dispatchToMainThread([=] {
				if (const auto strong = weak.get()) {
					strong->show(anim::type::normal);
				}
			}, st::widgetFadeDuration);
		}, musicButton->lifetime());
		musicButton->entity()->setCoverActive(container->isVisible());
		divider->toggle(true, anim::type::normal);
		state->hasMusic = true;
	}, container->lifetime());
	divider->finishAnimating();

	return state->hasMusic.value();
}

} // namespace Info::Saved
