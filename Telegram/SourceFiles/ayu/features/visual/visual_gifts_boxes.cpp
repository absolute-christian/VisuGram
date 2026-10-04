#include "ayu/features/visual/visual_gifts.h"

#include "api/api_premium.h"
#include "base/random.h"
#include "boxes/star_gift_box.h"
#include "boxes/star_gift_cover_box.h"
#include "chat_helpers/compose/compose_show.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "info/peer_gifts/info_peer_gifts_common.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "mainwindow.h"
#include "mtproto/mtproto_response.h"
#include "mtproto/sender.h"
#include "settings/settings_common.h"
#include "ui/boxes/boost_box.h"
#include "ui/layers/generic_box.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_credits.h"
#include "styles/style_settings.h"

#include <QtCore/QRegularExpression>
#include <QtCore/QUrl>

namespace Ayu::Visual {
namespace {

using Info::PeerGifts::GiftDescriptor;
using Info::PeerGifts::GiftTypeStars;

void StorageError(not_null<Window::SessionController*> window) {
	window->showToast(Text(
		u"Could not save visual settings. Check the local data folder."_q,
		u"Не удалось сохранить визуальные настройки. Проверьте папку данных."_q));
}

void AddLabel(not_null<Ui::GenericBox*> box, QString text) {
	box->addRow(object_ptr<Ui::FlatLabel>(
		box,
		rpl::single(std::move(text)),
		st::boxLabel), st::boxRowPadding);
}

void AddCover(
		not_null<Ui::GenericBox*> box,
		not_null<PeerData*> recipient,
		const Data::StarGift &gift) {
	if (gift.unique) {
		Ui::AddUniqueGiftCover(
			box->verticalLayout(),
			rpl::single(Ui::UniqueGiftCover{ *gift.unique }),
			{ .attributesInfo = true });
	} else {
		box->addRow(Ui::MakeVisualGiftPreview(
			box,
			recipient,
			GiftTypeStars{ .info = gift }));
	}
}

void ShowCatalogPage(
	not_null<Window::SessionController*> window,
	not_null<PeerData*> recipient,
	uint64 giftId,
	QString collectionTitle = QString());

void ShowSendBox(
		not_null<Window::SessionController*> window,
		not_null<PeerData*> recipient,
		MTPStarGift source) {
	const auto gift = Api::FromTL(&window->session(), source);
	if (!gift) {
		return;
	}
	window->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setWidth(st::boxWideWidth);
		box->setStyle(st::giftBox);
		box->setTitle(TextValue(u"Visual gift"_q, u"Визуальный подарок"_q));
		AddCover(box, recipient, *gift);
		AddLabel(box, Text(
			u"For "_q + recipient->name() + u" · visible only in this client"_q,
			u"Для "_q + recipient->name() + u" · виден только в этом клиенте"_q));
		const auto message = box->addRow(object_ptr<Ui::InputField>(
			box,
			st::giftBoxTextField,
			Ui::InputField::Mode::MultiLine,
			tr::lng_gift_send_message()), st::giftBoxTextPadding);
		message->setMaxLength(255);
		const auto anonymous = box->addRow(object_ptr<Ui::Checkbox>(
			box,
			tr::lng_gift_send_anonymous(tr::now),
			false,
			st::defaultCheckbox), st::boxRowPadding);
		if (!gift->unique && (gift->resellCount || gift->upgradable)) {
			box->addButton(TextValue(
				u"Collectible variants"_q,
				u"Коллекционные варианты"_q), [=] {
				ShowCatalogPage(window, recipient, gift->id, gift->resellTitle);
			});
		}
		box->addButton(tr::lng_gift_send_button(
			lt_cost,
			rpl::single(u"∞"_q)), [=] {
			if (!Enabled(&window->session())) {
				box->closeBox();
				return;
			}
			const auto saved = AddGift(
				recipient,
				source,
				message->getLastText(),
				anonymous->checked());
			if (!saved) {
				StorageError(window);
				return;
			}
			box->closeBox();
			window->hideLayer();
			window->showPeerHistory(
				recipient,
				Window::SectionShow::Way::ClearStack,
				ShowAtTheEndMsgId);
			window->showToast(Text(
				u"Visual gift sent"_q,
				u"Визуальный подарок отправлен"_q));
			Ui::StartFireworks(window->widget());
		});
		box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
	}));
}

void ImportGift(
		not_null<Window::SessionController*> window,
		not_null<PeerData*> recipient) {
	window->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(TextValue(
			u"Public collectible gift"_q,
			u"Публичный коллекционный подарок"_q));
		box->setWidth(st::boxWideWidth);
		AddLabel(box, Text(
			u"Paste a t.me/nft link or a gift slug, for example PlushPepe-1."_q,
			u"Вставьте ссылку t.me/nft или имя подарка, например PlushPepe-1."_q));
		const auto field = box->addRow(object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			Ui::InputField::Mode::SingleLine,
			rpl::single(u"https://t.me/nft/…"_q)), st::boxRowPadding);
		const auto sender = box->lifetime().make_state<MTP::Sender>(
			&window->session().mtp());
		const auto loading = box->lifetime().make_state<bool>(false);
		box->addButton(TextValue(u"Open"_q, u"Открыть"_q), [=] {
			if (*loading) {
				return;
			}
			auto slug = field->getLastText().trimmed();
			if (slug.contains('/')) {
				const auto url = QUrl::fromUserInput(slug);
				if (url.host() != u"t.me" || !url.path().startsWith(u"/nft/")) {
				field->showError();
				return;
				}
				slug = url.path().mid(5);
			}
			const auto pattern = QRegularExpression(u"^[A-Za-z0-9]+-[0-9]+$"_q);
			if (!pattern.match(slug).hasMatch()) {
				field->showError();
				return;
			}
			*loading = true;
			sender->request(MTPpayments_GetUniqueStarGift(
				MTP_string(slug)
			)).done([=](const MTPpayments_UniqueStarGift &result) {
				*loading = false;
				const auto &data = result.data();
				window->session().data().processUsers(data.vusers());
				if (!Api::FromTL(&window->session(), data.vgift())) {
					field->showError();
					return;
				}
				ShowSendBox(window, recipient, data.vgift());
			}).fail([=](const MTP::Error &error) {
				*loading = false;
				MTP::ShowErrorFallback(window->uiShow(), error);
			}).send();
		});
		box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
	}));
}

class CatalogState final {
public:
	explicit CatalogState(not_null<Main::Session*> session) : api(&session->mtp()) {
	}

	MTP::Sender api;
	std::map<uint64, MTPStarGift> sources;
	Ui::GiftsDescriptor gifts;
	rpl::event_stream<> updated;
	rpl::variable<QString> status;
	QString offset;
	bool loading = false;
	bool allLoaded = false;
	bool samplesRequested = false;

};

void ShowCatalogPage(
		not_null<Window::SessionController*> window,
		not_null<PeerData*> recipient,
		uint64 giftId,
		QString collectionTitle) {
	window->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setWidth(st::boxWideWidth);
		box->setStyle(st::giftBox);
		box->setTitle(TextValue(
			giftId ? u"Visual collectibles"_q : u"Visual gifts"_q,
			giftId ? u"Визуальные коллекционные подарки"_q : u"Визуальные подарки"_q));
		const auto session = &window->session();
		const auto state = box->lifetime().make_state<CatalogState>(session);
		state->status = tr::lng_contacts_loading(tr::now);
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			state->status.value(),
			st::boxLabel), st::boxRowPadding);
		const auto append = [=](const QVector<MTPStarGift> &sources) {
			for (const auto &source : sources) {
				if (const auto gift = Api::FromTL(session, source)) {
					if (!state->sources.emplace(gift->id, source).second) {
						continue;
					}
					state->gifts.list.push_back(GiftTypeStars{ .info = *gift });
				}
			}
			state->status = state->gifts.list.empty()
				? Text(u"No public gifts found."_q, u"Публичные подарки не найдены."_q)
				: QString();
			state->updated.fire({});
		};
		const auto failed = [=](const MTP::Error &error) {
			state->loading = false;
			state->status = Text(
				u"Could not load gifts. Use Retry."_q,
				u"Не удалось загрузить подарки. Нажмите «Повторить»."_q);
			MTP::ShowErrorFallback(window->uiShow(), error);
		};
		const auto load = [=] {
			if (state->loading
				|| (state->allLoaded && (!giftId || state->samplesRequested))) {
				return;
			}
			state->loading = true;
			if (giftId) {
				if (!state->samplesRequested) {
					state->samplesRequested = true;
					state->api.request(MTPpayments_GetStarGiftUpgradePreview(
						MTP_long(giftId)
					)).done([=](const MTPpayments_StarGiftUpgradePreview &result) {
						auto models = QVector<MTPStarGiftAttribute>();
						auto patterns = QVector<MTPStarGiftAttribute>();
						auto backdrops = QVector<MTPStarGiftAttribute>();
						for (const auto &attribute : result.data().vsample_attributes().v) {
							switch (attribute.type()) {
							case mtpc_starGiftAttributeModel: models.push_back(attribute); break;
							case mtpc_starGiftAttributePattern: patterns.push_back(attribute); break;
							case mtpc_starGiftAttributeBackdrop: backdrops.push_back(attribute); break;
							}
						}
						if (models.empty() || patterns.empty() || backdrops.empty()) {
							return;
						}
						auto samples = QVector<MTPStarGift>();
						const auto title = collectionTitle.isEmpty()
							? Text(u"Visual collectible"_q, u"Визуальный коллекционный подарок"_q)
							: collectionTitle;
						for (auto i = 0; i != models.size(); ++i) {
							const auto id = base::RandomValue<uint64>() & 0x7FFFFFFFFFFFFFFFULL;
							using Flag = MTPDstarGiftUnique::Flag;
							samples.push_back(MTP_starGiftUnique(
								MTP_flags(Flag::f_owner_id),
								MTP_long(id),
								MTP_long(giftId),
								MTP_string(title),
								MTP_string(u"Visual-"_q + QString::number(id)),
								MTP_int(i + 1),
								peerToMTP(recipient->id),
								MTP_string(QString()),
								MTP_string(QString()),
								MTP_vector<MTPStarGiftAttribute>({
									models[i],
									patterns[i % patterns.size()],
									backdrops[i % backdrops.size()],
								}),
								MTP_int(1),
								MTP_int(1),
								MTP_string(QString()),
								MTPVector<MTPStarsAmount>(),
								MTPPeer(),
								MTP_long(0),
								MTP_string(QString()),
								MTP_long(0),
								MTPPeer(),
								MTPPeerColor(),
								MTPPeer(),
								MTP_int(0),
								MTP_int(0)));
						}
						append(samples);
					}).fail([=](const MTP::Error &) {
						state->samplesRequested = false;
					}).send();
				}
				state->api.request(MTPpayments_GetResaleStarGifts(
					MTP_flags(0),
					MTP_long(0),
					MTP_long(giftId),
					MTPVector<MTPStarGiftAttributeId>(),
					MTP_string(state->offset),
					MTP_int(50)
				)).done([=](const MTPpayments_ResaleStarGifts &result) {
					state->loading = false;
					const auto &data = result.data();
					session->data().processUsers(data.vusers());
					session->data().processChats(data.vchats());
					state->offset = qs(data.vnext_offset().value_or_empty());
					state->allLoaded = state->offset.isEmpty();
					append(data.vgifts().v);
				}).fail(failed).send();
			} else {
				state->api.request(MTPpayments_GetStarGifts(
					MTP_int(0)
				)).done([=](const MTPpayments_StarGifts &result) {
					state->loading = false;
					state->allLoaded = true;
					if (result.type() == mtpc_payments_starGifts) {
						const auto &data = result.c_payments_starGifts();
						session->data().processUsers(data.vusers());
						session->data().processChats(data.vchats());
						append(data.vgifts().v);
					}
				}).fail(failed).send();
			}
		};
		box->addRow(Ui::MakeGiftsList({
			.window = window,
			.peer = recipient,
			.gifts = rpl::single(rpl::empty) | rpl::then(
				state->updated.events()
			) | rpl::map([=] { return state->gifts; }),
			.loadMore = load,
			.handler = [=](GiftDescriptor descriptor) {
				if (!Enabled(session)) {
					box->closeBox();
					return;
				}
				const auto gift = std::get_if<GiftTypeStars>(&descriptor);
				const auto source = gift
					? state->sources.find(gift->info.id)
					: end(state->sources);
				if (source != end(state->sources)) {
					ShowSendBox(window, recipient, source->second);
				}
			},
		}));
		box->addButton(TextValue(u"Public NFT link"_q, u"Ссылка на NFT"_q), [=] {
			ImportGift(window, recipient);
		});
		box->addButton(TextValue(u"Retry / More"_q, u"Повторить / Ещё"_q), load);
		box->addButton(tr::lng_close(), [=] { box->closeBox(); });
		load();
	}));
}

void EditProfile(not_null<Window::SessionController*> window, bool editPhone) {
	window->show(Box([=](not_null<Ui::GenericBox*> box) {
		const auto session = &window->session();
		box->setWidth(st::boxWideWidth);
		box->setTitle(TextValue(
			editPhone ? u"Visual phone number"_q : u"Visual NFT usernames"_q,
			editPhone ? u"Визуальный номер телефона"_q : u"Визуальные NFT-юзернеймы"_q));
		AddLabel(box, Text(
			u"Visible only in this client. Leave empty to use your real profile."_q,
			u"Видно только в этом клиенте. Оставьте пустым для настоящих данных профиля."_q));
		if (!editPhone) {
			AddLabel(box, Text(
				u"One username per line. The first is primary; reorder lines to change it."_q,
				u"Один юзернейм на строку. Первый — основной; порядок можно менять."_q));
		}
		const auto field = box->addRow(object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			editPhone ? Ui::InputField::Mode::SingleLine : Ui::InputField::Mode::MultiLine,
			rpl::single(editPhone ? u"+888 …"_q : u"@username"_q),
			editPhone ? Phone(session) : Usernames(session).join('\n')),
			st::boxRowPadding);
		field->setMaxLength(editPhone ? 64 : 1024);
		box->addButton(tr::lng_settings_save(), [=] {
			const auto text = field->getLastText().trimmed();
			auto names = QStringList();
			if (!editPhone) {
				const auto pattern = QRegularExpression(u"^[A-Za-z][A-Za-z0-9_]{3,31}$"_q);
				for (auto name : text.split('\n', Qt::SkipEmptyParts)) {
					name = name.trimmed();
					if (name.startsWith('@')) {
						name.remove(0, 1);
					}
					if (!pattern.match(name).hasMatch()
						|| names.contains(name, Qt::CaseInsensitive)
						|| names.size() >= 20) {
						field->showError();
						return;
					}
					names.push_back(name);
				}
			}
			if (!SetProfile(session,
				editPhone ? text : Phone(session),
				editPhone ? Usernames(session) : names)) {
				StorageError(window);
				return;
			}
			box->closeBox();
		});
		box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
	}));
}

}

void ShowCatalog(
		not_null<Window::SessionController*> window,
		not_null<PeerData*> recipient) {
	ShowCatalogPage(window, recipient, 0);
}

void ShowLocalGift(
		not_null<Window::SessionController*> window,
		const Data::SavedStarGift &gift) {
	window->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setWidth(st::boxWideWidth);
		box->setStyle(st::giftBox);
		box->setTitle(TextValue(u"Visual gift"_q, u"Визуальный подарок"_q));
		AddCover(box, window->session().user(), gift.info);
		if (const auto &unique = gift.info.unique) {
			AddLabel(box,
				Text(u"Model: "_q, u"Модель: "_q) + unique->model.name
				+ '\n' + Text(u"Backdrop: "_q, u"Фон: "_q) + unique->backdrop.name
				+ '\n' + Text(u"Symbol: "_q, u"Узор: "_q) + unique->pattern.name);
		}
		if (!gift.message.empty()) {
			AddLabel(box, gift.message.text);
		}
		const auto session = &window->session();
		const auto id = gift.manageId;
		if (gift.info.unique) {
			box->addButton(TextValue(
				gift.pinned ? u"Unpin from profile"_q : u"Pin to profile"_q,
				gift.pinned ? u"Открепить от профиля"_q : u"Закрепить в профиле"_q), [=] {
				if (!SetPinned(session, id, !gift.pinned)) {
					window->showToast(Text(
						u"Could not pin gift. Up to six gifts can be pinned."_q,
						u"Не удалось закрепить подарок. Можно закрепить до шести подарков."_q));
					return;
				}
				box->closeBox();
			});
		}
		box->addButton(TextValue(
			gift.hidden ? u"Show on profile"_q : u"Hide from profile"_q,
			gift.hidden ? u"Показать в профиле"_q : u"Скрыть из профиля"_q), [=] {
			if (!SetHidden(session, id, !gift.hidden)) {
				StorageError(window);
				return;
			}
			box->closeBox();
		});
		box->addButton(TextValue(u"Delete visual gift"_q, u"Удалить визуальный подарок"_q), [=] {
			if (!RemoveGift(session, id)) {
				StorageError(window);
				return;
			}
			box->closeBox();
		});
		box->addButton(tr::lng_close(), [=] { box->closeBox(); });
	}));
}

void AddProfileRows(
		not_null<Ui::VerticalLayout*> container,
		not_null<Window::SessionController*> window) {
	Settings::AddButtonWithIcon(container, TextValue(
		u"Visual phone number"_q,
		u"Визуальный номер телефона"_q), st::settingsButton)->setClickedCallback([=] {
		EditProfile(window, true);
	});
	Settings::AddButtonWithIcon(container, TextValue(
		u"Visual NFT usernames"_q,
		u"Визуальные NFT-юзернеймы"_q), st::settingsButton)->setClickedCallback([=] {
		EditProfile(window, false);
	});
}

void ShowProfileEditor(
		not_null<Window::SessionController*> window,
		bool editPhone) {
	EditProfile(window, editPhone);
}

}
