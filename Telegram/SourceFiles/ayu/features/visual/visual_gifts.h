#pragma once

#include "data/data_star_gift.h"
#include "data/data_types.h"
#include <QtCore/QJsonObject>

class History;

namespace Main {
class Session;
}

namespace Window {
class SessionController;
}

namespace Ui {
class GenericBox;
class VerticalLayout;
}

namespace Ayu::Visual {

[[nodiscard]] QString Text(QString english, QString russian);
[[nodiscard]] rpl::producer<QString> TextValue(
	QString english,
	QString russian);
[[nodiscard]] bool Enabled(not_null<Main::Session*> session);
[[nodiscard]] rpl::producer<bool> EnabledValue(
	not_null<Main::Session*> session);
[[nodiscard]] rpl::producer<> Changes(not_null<Main::Session*> session);
[[nodiscard]] bool SetEnabled(not_null<Main::Session*> session, bool enabled);
[[nodiscard]] QString Phone(not_null<Main::Session*> session);
[[nodiscard]] QStringList Usernames(not_null<Main::Session*> session);
[[nodiscard]] QString DefaultSyncServer();
[[nodiscard]] QString SyncServer(not_null<Main::Session*> session);
[[nodiscard]] bool SetSyncServer(not_null<Main::Session*> session, QString endpoint);
[[nodiscard]] QString PhoneFor(not_null<PeerData*> peer);
[[nodiscard]] QStringList DisplayUsernames(not_null<PeerData*> peer);
[[nodiscard]] bool IsVisualUsername(not_null<PeerData*> peer, QString name);
[[nodiscard]] QJsonObject CollectibleMetadata(not_null<PeerData*> peer, QString entity);
[[nodiscard]] QString SyncError(QString code);
void SaveProfile(not_null<Main::Session*> session,
	QString phone, QStringList names, QString primary, Fn<void(QString)> done);
void SendGift(not_null<PeerData*> recipient, const MTPStarGift &source,
	QString message, bool anonymous, CreditsAmount price, QString operation,
	Fn<void(std::optional<Data::SavedStarGift>, QString)> done);
void ManageGift(not_null<Main::Session*> session, Data::SavedStarGiftId id,
	bool pinned, bool hidden, Fn<void(QString)> done);
void UpdateGift(not_null<Main::Session*> session, Data::SavedStarGiftId id,
	QJsonObject changes, Fn<void(QString)> done);
void TransferGift(not_null<Main::Session*> session, Data::SavedStarGiftId id,
	not_null<PeerData*> recipient, Fn<void(QString)> done);
void BuyGift(not_null<Main::Session*> session, Data::SavedStarGiftId id,
	Fn<void(QString)> done);
[[nodiscard]] bool GiftWorn(
	not_null<Main::Session*> session, Data::SavedStarGiftId id);
[[nodiscard]] EmojiStatusId WornStatus(not_null<const PeerData*> peer);
void ShowCollectible(not_null<Window::SessionController*> window,
	not_null<PeerData*> peer, QString entity);
void ShowSyncSettings(not_null<Window::SessionController*> window);
[[nodiscard]] bool SetProfile(
	not_null<Main::Session*> session,
	QString phone,
	QStringList usernames);
[[nodiscard]] std::vector<Data::SavedStarGift> Gifts(
	not_null<PeerData*> peer,
	bool pinnedOnly = false);
[[nodiscard]] int GiftCount(not_null<PeerData*> peer);
[[nodiscard]] bool IsSyncedGift(
	not_null<Main::Session*> session, Data::SavedStarGiftId id);
[[nodiscard]] bool IsLocal(
	not_null<Main::Session*> session,
	Data::SavedStarGiftId id);
[[nodiscard]] std::optional<Data::SavedStarGift> FindGift(
	not_null<Main::Session*> session,
	Data::SavedStarGiftId id);
[[nodiscard]] PeerId GiftRecipient(
	not_null<Main::Session*> session,
	Data::SavedStarGiftId id);
[[nodiscard]] std::optional<Data::SavedStarGift> AddGift(
	not_null<PeerData*> recipient,
	const MTPStarGift &gift,
	QString message,
	bool anonymous,
	CreditsAmount price = CreditsAmount(),
	bool localOnly = false);
[[nodiscard]] bool SetPinned(
	not_null<Main::Session*> session,
	Data::SavedStarGiftId id,
	bool pinned);
[[nodiscard]] bool SetHidden(
	not_null<Main::Session*> session,
	Data::SavedStarGiftId id,
	bool hidden);
[[nodiscard]] bool RemoveGift(
	not_null<Main::Session*> session,
	Data::SavedStarGiftId id);
void RestoreMessages(not_null<Main::Session*> session);
void RestoreHistory(not_null<History*> history);
void RefreshGift(
	not_null<Main::Session*> session,
	Data::SavedStarGiftId id);
void ShowProfileEditor(
	not_null<Window::SessionController*> window,
	bool editPhone);
void ShowCatalog(
	not_null<Window::SessionController*> window,
	not_null<PeerData*> recipient);
void ShowPurchase(
	not_null<Window::SessionController*> window,
	not_null<PeerData*> recipient,
	const Data::StarGift &gift,
	bool forceTon = false);
void ShowImport(
	not_null<Window::SessionController*> window,
	not_null<PeerData*> recipient);
void ShowLocalGift(
	not_null<Window::SessionController*> window,
	const Data::SavedStarGift &gift);
void AddProfileRows(
	not_null<Ui::VerticalLayout*> container,
	not_null<Window::SessionController*> window);

}
