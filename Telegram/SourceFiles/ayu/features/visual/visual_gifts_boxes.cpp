#include "ayu/features/visual/visual_gifts.h"

#include "api/api_premium.h"
#include "base/unixtime.h"
#include "boxes/gift_premium_box.h"
#include "boxes/peer_list_controllers.h"
#include "boxes/star_gift_box.h"
#include "boxes/star_gift_cover_box.h"
#include "chat_helpers/compose/compose_show.h"
#include "data/data_credits.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "history/view/controls/history_view_suggest_options.h"
#include "info/peer_gifts/info_peer_gifts_common.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "main/main_app_config.h"
#include "mainwindow.h"
#include "mtproto/mtproto_response.h"
#include "mtproto/sender.h"
#include "settings/settings_common.h"
#include "settings/settings_credits_graphics.h"
#include "ui/boxes/boost_box.h"
#include "ui/boxes/collectible_info_box.h"
#include "ui/boxes/confirm_box.h"
#include "ui/dynamic_thumbnails.h"
#include "ui/layers/generic_box.h"
#include "ui/text/custom_emoji_helper.h"
#include "ui/text/format_values.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/popup_menu.h"
#include "ui/text/text_utilities.h"
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_chat.h"
#include "styles/style_credits.h"
#include "styles/style_giveaway.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"

#include <QtCore/QRegularExpression>
#include <QtCore/QUrl>
#include <QtCore/QUuid>

namespace Ayu::Visual {
namespace {

using Info::PeerGifts::GiftTypeStars;

void AddLabel(not_null<Ui::GenericBox*> box, QString text);

class GiftRecipientController final : public ContactsBoxController {
public:
	GiftRecipientController(
		not_null<Main::Session*> session,
		Fn<void(not_null<PeerData*>)> choose)
	: ContactsBoxController(session)
	, _choose(std::move(choose)) {
	}

	void rowClicked(not_null<PeerListRow*> row) override {
		_choose(row->peer());
	}

protected:
	void prepareViewHook() override {
		delegate()->peerListSetTitle(tr::lng_gift_transfer_choose());
	}

	std::unique_ptr<PeerListRow> createRow(not_null<UserData*> user) override {
		return (user->isSelf() || user->isBot() || user->isServiceUser()
			|| user->isInaccessible())
			? nullptr : ContactsBoxController::createRow(user);
	}

private:
	Fn<void(not_null<PeerData*>)> _choose;

};

void ShowGiftTransfer(
		not_null<Window::SessionController*> window,
		Data::SavedStarGift gift) {
	const auto choose = [=](not_null<PeerData*> recipient) {
		window->show(Box([=](not_null<Ui::GenericBox*> box) {
			box->setStyle(st::giveawayGiftCodeBox);
			box->setWidth(st::boxWideWidth);
			Ui::AddUniqueGiftCover(box->verticalLayout(),
				rpl::single(Ui::UniqueGiftCover{ *gift.info.unique }), {});
			const auto busy = box->lifetime().make_state<bool>(false);
			Ui::ConfirmBox(box, {
				.text = tr::lng_gift_transfer_sure(
					lt_name,
					rpl::single(tr::bold(Data::UniqueGiftName(*gift.info.unique))),
					lt_recipient,
					rpl::single(tr::bold(recipient->shortName())),
					tr::marked),
				.confirmed = [=] {
					if (std::exchange(*busy, true)) {
						return;
					}
					TransferGift(&window->session(), gift.manageId, recipient,
						crl::guard(box, [=](QString error) {
							*busy = false;
							if (!error.isEmpty()) {
								window->showToast(SyncError(error));
								return;
							}
							window->hideLayer();
							window->showPeerHistory(recipient);
						}));
				},
				.confirmText = tr::lng_gift_transfer_button(),
			});
			AddTransferGiftTable(window->uiShow(), box->verticalLayout(), gift.info.unique);
		}), Ui::LayerOption::KeepOther);
	};
	window->show(Box<PeerListBox>(
		std::make_unique<GiftRecipientController>(&window->session(), choose),
		[](not_null<PeerListBox*> box) {
			box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
		}), Ui::LayerOption::KeepOther);
}

void ShowGiftSale(
		not_null<Window::SessionController*> window,
		Data::SavedStarGift gift) {
	window->show(Box([=](not_null<Ui::GenericBox*> box) {
		const auto session = &window->session();
		const auto &config = session->appConfig();
		box->setStyle(st::upgradeGiftBox);
		box->setWidth(st::boxWideWidth);
		box->addTopButton(st::boxTitleClose, [=] { box->closeBox(); });
		const auto ton = box->lifetime().make_state<rpl::variable<bool>>(
			gift.info.unique->onlyAcceptTon);
		box->setTitle(rpl::conditional(ton->value(),
			tr::lng_gift_sell_title_ton(), tr::lng_gift_sell_title()));
		auto initial = Data::UniqueGiftResaleAsked(*gift.info.unique);
		if (initial.value() <= 0) {
			initial = CreditsAmount(config.giftResaleStarsMin());
		}
		auto input = HistoryView::AddStarsTonPriceInput(box->verticalLayout(), {
			.session = session,
			.showTon = ton->value(),
			.price = initial,
			.starsMin = config.giftResaleStarsMin(),
			.starsMax = config.giftResaleStarsMax(),
			.nanoTonMin = config.giftResaleNanoTonMin(),
			.nanoTonMax = config.giftResaleNanoTonMax(),
		});
		box->setFocusCallback(std::move(input.focusCallback));
		Ui::AddSkip(box->verticalLayout());
		const auto onlyTon = box->addRow(object_ptr<Ui::Checkbox>(box,
			tr::lng_gift_sell_only_ton(tr::now), ton->current(), st::defaultCheckbox));
		*ton = onlyTon->checkedValue();
		AddLabel(box, Text(
			u"The gift will be offered to other VisuGram users at this visual price."_q,
			u"Подарок будет выставлен для других пользователей VisuGram по этой визуальной цене."_q));
		const auto busy = box->lifetime().make_state<bool>(false);
		box->addButton(tr::lng_settings_save(), [=, compute = std::move(input.computeResult)] {
			const auto price = compute();
			if (*busy || !price || price->value() <= 0) {
				return;
			}
			*busy = true;
			UpdateGift(session, gift.manageId, {
				{ u"sale_price"_q, QString::number(price->ton()
					? price->whole() * 1'000'000'000 + price->nano() : price->whole()) },
				{ u"sale_currency"_q, price->ton() ? u"TON"_q : u"XTR"_q },
			}, crl::guard(box, [=](QString error) {
				*busy = false;
				if (!error.isEmpty()) {
					window->showToast(SyncError(error));
					return;
				}
				window->hideLayer();
				const auto updated = FindGift(session, gift.manageId);
				if (updated) {
					ShowLocalGift(window, *updated);
				}
			}));
		});
		box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
	}), Ui::LayerOption::KeepOther);
}

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

void ShowSendBox(
		not_null<Window::SessionController*> window,
		not_null<PeerData*> recipient,
		MTPStarGift source,
		bool forceTon = false) {
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
			u"Visual gift for "_q + recipient->name(),
			u"Визуальный подарок для "_q + recipient->name()));
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
		auto price = gift->unique
			? (forceTon
				? Data::UniqueGiftResaleTon(*gift->unique)
				: Data::UniqueGiftResaleAsked(*gift->unique))
			: CreditsAmount(gift->stars);
		if (price.value() < 0) {
			price = CreditsAmount();
		}
		const auto sending = box->lifetime().make_state<bool>(false);
		const auto operation = box->lifetime().make_state<QString>();
		const auto lastInput = box->lifetime().make_state<QString>();
		box->addButton(tr::lng_gift_send_button(
			lt_cost,
			rpl::single((price.ton() ? u"TON "_q : u"★ "_q)
				+ Lang::FormatCreditsAmountDecimal(price))), [=] {
			if (*sending || !Enabled(&window->session())) {
				return;
			}
			const auto text = message->getLastText();
			const auto fingerprint = text + (anonymous->checked() ? '1' : '0');
			if (operation->isEmpty() || *lastInput != fingerprint) {
				*operation = QUuid::createUuid().toString(QUuid::WithoutBraces);
				*lastInput = fingerprint;
			}
			*sending = true;
			SendGift(recipient, source, text, anonymous->checked(), price, *operation,
				crl::guard(box, [=](std::optional<Data::SavedStarGift> saved, QString error) {
					*sending = false;
					if (!saved) {
						window->showToast(SyncError(error));
						return;
					}
					box->closeBox();
					window->hideLayer();
					window->showPeerHistory(recipient,
						Window::SectionShow::Way::ClearStack, ShowAtTheEndMsgId);
					window->showToast(Text(u"Visual gift sent"_q,
						u"Визуальный подарок отправлен"_q));
					Ui::StartFireworks(window->widget());
				}));
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

void EditProfile(not_null<Window::SessionController*> window, bool editPhone) {
	window->show(Box([=](not_null<Ui::GenericBox*> box) {
		const auto session = &window->session();
		box->setWidth(st::boxWideWidth);
		box->setTitle(TextValue(
			editPhone ? u"Visual phone number"_q : u"Visual NFT usernames"_q,
			editPhone ? u"Визуальный номер телефона"_q : u"Визуальные NFT-юзернеймы"_q));
		AddLabel(box, Text(
			editPhone ? u"Only existing collectible +888 numbers are supported."_q
				: u"One username per line. Put a collectible first to display it as primary. Your Telegram usernames remain available."_q,
			editPhone ? u"Поддерживаются только существующие коллекционные номера +888."_q
				: u"Один юзернейм на строку. Поставьте коллекционный первым, чтобы сделать его основным. Настоящие юзернеймы Telegram сохранятся."_q));
		auto initialNames = DisplayUsernames(session->user());
		for (const auto &name : Usernames(session)) {
			if (!initialNames.contains(name, Qt::CaseInsensitive)) {
				initialNames.push_back(name);
			}
		}
		const auto field = box->addRow(object_ptr<Ui::InputField>(
			box, st::defaultInputField,
			editPhone ? Ui::InputField::Mode::SingleLine : Ui::InputField::Mode::MultiLine,
			rpl::single(editPhone ? u"+888 …"_q : u"@username"_q),
			editPhone ? (Phone(session).isEmpty() ? u"+888"_q : Phone(session))
				: initialNames.join('\n')), st::boxRowPadding);
		field->setMaxLength(editPhone ? 32 : 1024);
		const auto saving = box->lifetime().make_state<bool>(false);
		box->addButton(tr::lng_settings_save(), [=] {
			if (*saving) {
				return;
			}
			const auto text = field->getLastText().trimmed();
			auto phone = Phone(session);
			auto names = Usernames(session);
			auto primary = IsVisualUsername(session->user(), initialNames.value(0))
				? initialNames.value(0) : QString();
			if (editPhone) {
				if (!QRegularExpression(u"^[+0-9 ()-]*$"_q).match(text).hasMatch()) {
					field->showError();
					return;
				}
				phone = text;
				phone.remove(QRegularExpression(u"[^0-9]"_q));
				if (phone == u"888" || phone.isEmpty()) {
					phone.clear();
				} else {
					if (phone.size() == 8) {
						phone.prepend(u"888"_q);
					}
					if (!QRegularExpression(u"^888[0-9]{8}$"_q).match(phone).hasMatch()) {
						field->showError();
						return;
					}
					phone.prepend('+');
				}
			} else {
				names.clear();
				primary.clear();
				auto seen = QStringList();
				const auto pattern = QRegularExpression(u"^[a-z][a-z0-9_]{3,31}$"_q);
				for (auto name : text.split('\n', Qt::SkipEmptyParts)) {
					name = name.trimmed().toLower();
					if (name.startsWith('@')) {
						name.remove(0, 1);
					}
					if (!pattern.match(name).hasMatch()
						|| seen.contains(name) || seen.size() >= 20) {
						field->showError();
						return;
					}
					const auto native = ranges::any_of(session->user()->usernames(),
						[&](const QString &value) { return value.compare(name, Qt::CaseInsensitive) == 0; });
					if (!native && name != session->user()->editableUsername().toLower()) {
						if (seen.isEmpty()) {
							primary = name;
						}
						names.push_back(name);
					}
					seen.push_back(name);
				}
			}
			*saving = true;
			SaveProfile(session, phone, names, primary, crl::guard(box, [=](QString error) {
				*saving = false;
				if (!error.isEmpty()) {
					window->showToast(SyncError(error));
					return;
				}
				box->closeBox();
			}));
		});
		box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
	}));
}

}

void ShowCatalog(
		not_null<Window::SessionController*> window,
		not_null<PeerData*> recipient) {
	Ui::ShowStarGiftBox(window, recipient);
}

void ShowPurchase(
		not_null<Window::SessionController*> window,
		not_null<PeerData*> recipient,
		const Data::StarGift &gift,
		bool forceTon) {
	if (!Enabled(&window->session())) {
		return;
	}
	window->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setWidth(st::boxWideWidth);
		box->setTitle(tr::lng_gift_send_title());
		AddLabel(box, tr::lng_contacts_loading(tr::now));
		const auto session = &window->session();
		const auto sender = box->lifetime().make_state<MTP::Sender>(
			&session->mtp());
		const auto open = [=](const MTPStarGift &source) {
			if (!Enabled(session)) {
				box->closeBox();
				return;
			}
			box->closeBox();
			ShowSendBox(window, recipient, source, forceTon);
		};
		const auto failed = [=](const MTP::Error &error) {
			MTP::ShowErrorFallback(window->uiShow(), error);
			box->closeBox();
		};
		if (gift.unique) {
			sender->request(MTPpayments_GetUniqueStarGift(
				MTP_string(gift.unique->slug)
			)).done([=](const MTPpayments_UniqueStarGift &result) {
				session->data().processUsers(result.data().vusers());
				open(result.data().vgift());
			}).fail(failed).send();
		} else {
			sender->request(MTPpayments_GetStarGifts(
				MTP_int(0)
			)).done([=](const MTPpayments_StarGifts &result) {
				if (result.type() == mtpc_payments_starGifts) {
					const auto &data = result.c_payments_starGifts();
					session->data().processUsers(data.vusers());
					session->data().processChats(data.vchats());
					for (const auto &source : data.vgifts().v) {
						if (source.type() == mtpc_starGift
							&& uint64(source.c_starGift().vid().v) == gift.id) {
							open(source);
							return;
						}
					}
				}
				window->showToast(Text(
					u"Gift is no longer in the catalog."_q,
					u"Подарка больше нет в каталоге."_q));
				box->closeBox();
			}).fail(failed).send();
		}
		box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
	}));
}

void ShowImport(
		not_null<Window::SessionController*> window,
		not_null<PeerData*> recipient) {
	ImportGift(window, recipient);
}

void ShowLocalGift(
		not_null<Window::SessionController*> window,
		const Data::SavedStarGift &gift) {
	window->show(Box([=](not_null<Ui::GenericBox*> box) {
		const auto session = &window->session();
		const auto id = gift.manageId;
		const auto unique = gift.info.unique;
		const auto canManage = gift.mine
			&& (SyncServer(session).isEmpty() || IsSyncedGift(session, id));
		box->setStyle(st::giveawayGiftCodeBox);
		box->setWidth(st::boxWideWidth);
		const auto recipientId = GiftRecipient(session, id);
		const auto recipient = recipientId
			? session->data().peer(recipientId) : session->user();
		RefreshGift(session, id);
		if (unique) {
			box->setNoContentMargin(true);
			Ui::AddUniqueGiftCover(box->verticalLayout(),
				rpl::single(Ui::UniqueGiftCover{ *unique }), {
					.numberText = rpl::single(u"#"_q
						+ Lang::FormatCountDecimal(unique->number)),
					.resalePrice = rpl::single(Data::UniqueGiftResaleAsked(*unique)),
				});
		} else {
			AddCover(box, recipient, gift.info);
		}
		Ui::AddSkip(box->verticalLayout());
		AddStarGiftTable(window->uiShow(), box->verticalLayout(), {},
			Settings::SavedStarGiftEntry(recipient, gift), nullptr, nullptr,
			false, nullptr);
		Ui::AddSkip(box->verticalLayout());
		const auto managing = box->lifetime().make_state<bool>(false);
		const auto updated = crl::guard(box, [=](QString error) {
			*managing = false;
			if (!error.isEmpty()) {
				window->showToast(SyncError(error));
				return;
			}
			box->closeBox();
			if (const auto current = FindGift(session, id)
				; current && current->mine) {
				ShowLocalGift(window, *current);
			}
		});
		const auto update = [=](QJsonObject changes) {
			if (!std::exchange(*managing, true)) {
				UpdateGift(session, id, std::move(changes), updated);
			}
		};
		const auto manage = [=](bool pinned, bool hidden) {
			if (!std::exchange(*managing, true)) {
				ManageGift(session, id, pinned, hidden, updated);
			}
		};
		if (canManage) {
			const auto hint = gift.hidden
				? tr::lng_gift_hidden_unique(tr::now)
				: tr::lng_gift_visible_hint(tr::now);
			const auto arrow = Ui::Text::IconEmoji(&st::textMoreIconEmoji);
			const auto action = gift.hidden
				? tr::lng_gift_visible_show_arrow(tr::now, lt_arrow, arrow, tr::marked)
				: tr::lng_gift_visible_hide_arrow(tr::now, lt_arrow, arrow, tr::marked);
			auto label = object_ptr<Ui::FlatLabel>(box,
				rpl::single(TextWithEntities{ hint }.append(' ').append(
					tr::link(action))), st::creditsBoxAboutDivider);
			label->setClickHandlerFilter([=](const auto &, Qt::MouseButton button) {
				if (button != Qt::LeftButton) {
					return false;
				}
				manage(false, !gift.hidden);
				return true;
			});
			box->addRow(std::move(label), style::al_top);
		}
		Settings::AddUniqueCloseMoreButton(box, {}, [=](not_null<Ui::PopupMenu*> menu) {
			if (unique && canManage) {
				menu->addAction(tr::lng_gift_transfer_button(tr::now), [=] {
					ShowGiftTransfer(window, gift);
				}, &st::menuIconReplace);
				const auto worn = GiftWorn(session, id);
				menu->addAction((worn ? tr::lng_gift_transfer_take_off
					: tr::lng_gift_transfer_wear)(tr::now), [=] {
					update({ { u"worn"_q, !worn } });
				}, worn ? &st::menuIconNftTakeOff : &st::menuIconNftWear);
				const auto listed = Data::UniqueGiftResaleAsked(*unique).value() > 0;
				menu->addAction((listed ? tr::lng_gift_transfer_update
					: tr::lng_gift_transfer_sell)(tr::now), [=] {
					ShowGiftSale(window, gift);
				}, &st::menuIconTagSell);
				if (listed) {
					menu->addAction(tr::lng_gift_transfer_unlist(tr::now), [=] {
						update({ { u"listed"_q, false } });
					}, &st::menuIconTagRemove);
				}
				menu->addAction((gift.pinned ? tr::lng_context_unpin_from_top
					: tr::lng_context_pin_to_top)(tr::now), [=] {
					manage(!gift.pinned, false);
				}, gift.pinned ? &st::menuIconUnpin : &st::menuIconPin);
			}
			if (unique && !unique->slug.isEmpty()) {
				menu->addAction(tr::lng_context_copy_link(tr::now), [=] {
					TextUtilities::SetClipboardText({ session->createInternalLinkFull(
						u"nft/"_q + unique->slug) });
				}, &st::menuIconLink);
			}
			if (canManage) {
				menu->addAction(Text(u"Delete visual gift"_q,
					u"Удалить визуальный подарок"_q), [=] {
					window->show(Ui::MakeConfirmBox({
						.text = TextValue(u"Delete this visual gift?"_q,
							u"Удалить этот визуальный подарок?"_q),
						.confirmed = crl::guard(box, [=](Fn<void()> close) {
							close();
							update({ { u"removed"_q, true } });
						}),
						.confirmText = tr::lng_box_delete(),
					}), Ui::LayerOption::KeepOther);
				}, &st::menuIconDelete);
			}
		});
		if (!gift.mine && unique && Data::UniqueGiftResaleAsked(*unique).value() > 0) {
			const auto price = Data::UniqueGiftResaleAsked(*unique);
			const auto cost = Lang::FormatCreditsAmountDecimal(price)
				+ (price.ton() ? u" TON"_q : Text(u" Stars"_q, u" звёзд"_q));
			box->addButton(tr::lng_gift_buy_resale_button(lt_cost, rpl::single(cost)), [=] {
				window->show(Ui::MakeConfirmBox({
					.text = tr::lng_gift_buy_resale_confirm_self(
						lt_name, rpl::single(Data::UniqueGiftName(*unique)),
						lt_price, rpl::single(cost)),
					.confirmed = crl::guard(box, [=](Fn<void()> close) {
						close();
						if (!std::exchange(*managing, true)) {
							BuyGift(session, id, crl::guard(box, [=](QString error) {
								*managing = false;
								if (!error.isEmpty()) {
									window->showToast(SyncError(error));
								} else {
									window->hideLayer();
									window->showPeerHistory(session->user());
								}
							}));
						}
					}),
				}), Ui::LayerOption::KeepOther);
			});
		}
		box->addButton(tr::lng_box_ok(), [=] { box->closeBox(); });
	}));
}

void ShowCollectible(
		not_null<Window::SessionController*> window,
		not_null<PeerData*> peer,
		QString entity) {
	const auto metadata = CollectibleMetadata(peer, entity);
	if (metadata.isEmpty()) {
		window->showToast(SyncError(u"SERVER_REQUIRED"_q));
		return;
	}
	const auto amount = metadata.value(u"crypto_amount"_q).toString().toULongLong();
	const auto price = Lang::FormatCreditsAmountDecimal(CreditsAmount(
		amount / 1'000'000'000, amount % 1'000'000'000, CreditsType::Ton)) + u" TON"_q;
	const auto date = metadata.value(u"date"_q).toInt();
	const auto formattedDate = langDateTime(base::unixtime::parse(date));
	const auto kind = metadata.value(u"price_kind"_q).toString();
	const auto description = kind == u"visual"
		? Text(u"Visual purchase on %1 for %2."_q,
			u"Визуальная покупка %1 за %2."_q).arg(formattedDate, price)
		: kind == u"last_sale"
		? Text(u"Last sale on Fragment: %1, %2."_q,
			u"Последняя продажа на Fragment: %1, %2."_q).arg(price, formattedDate)
		: kind == u"asking"
		? Text(u"For sale on Fragment for %1."_q,
			u"Выставлен на Fragment за %1."_q).arg(price)
		: kind == u"bid"
		? Text(u"Current auction bid on Fragment: %1."_q,
			u"Текущая ставка на Fragment: %1."_q).arg(price)
		: Text(u"Minimum bid on Fragment: %1."_q,
			u"Минимальная ставка на Fragment: %1."_q).arg(price);
	window->show(Box(Ui::CollectibleInfoBox, Ui::CollectibleInfo{
		.entity = entity,
		.copyText = entity.startsWith('+') ? entity
			: peer->session().createInternalLinkFull(entity),
		.ownerUserpic = Ui::MakeUserpicThumbnail(peer, true),
		.ownerName = peer->name(),
		.cryptoAmount = amount,
		.amount = uint64(metadata.value(u"fiat_amount"_q).toDouble()),
		.cryptoCurrency = u"TON"_q,
		.currency = metadata.value(u"fiat_currency"_q).toString(),
		.url = metadata.value(u"url"_q).toString(),
		.date = date,
		.priceDescription = description,
	}));
}

void ShowSyncSettings(not_null<Window::SessionController*> window) {
	window->show(Box([=](not_null<Ui::GenericBox*> box) {
		const auto session = &window->session();
		box->setTitle(TextValue(u"Visual sync server"_q, u"Сервер синхронизации"_q));
		box->setWidth(st::boxWideWidth);
		AddLabel(box, Text(
			u"Synchronizes your Telegram ID, basic username, visual profile and gifts with this server. Visual profiles are visible to other users connected to it. Leave empty to disconnect."_q,
			u"Передаёт этому серверу Telegram ID, настоящий юзернейм, визуальный профиль и подарки. Профиль виден другим подключённым пользователям. Оставьте поле пустым для отключения."_q));
		const auto field = box->addRow(object_ptr<Ui::InputField>(
			box, st::defaultInputField, Ui::InputField::Mode::SingleLine,
			rpl::single(u"https://…"_q), SyncServer(session)), st::boxRowPadding);
		field->setMaxLength(512);
		box->addButton(tr::lng_settings_save(), [=] {
			if (!SetSyncServer(session, field->getLastText())) {
				field->showError();
				window->showToast(Text(u"Enter a valid HTTPS server address."_q,
					u"Введите корректный HTTPS-адрес сервера."_q));
				return;
			}
			box->closeBox();
		});
		box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
	}));
}

void AddProfileRows(
		not_null<Ui::VerticalLayout*> container,
		not_null<Window::SessionController*> window) {
	Settings::AddButtonWithIcon(container, TextValue(
		u"Visual sync server"_q, u"Сервер синхронизации"_q), st::settingsButton)->setClickedCallback([=] {
		ShowSyncSettings(window);
	});
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
