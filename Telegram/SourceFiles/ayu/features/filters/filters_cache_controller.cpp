// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "ayu/features/filters/filters_cache_controller.h"

#include "ayu/ayu_settings.h"
#include "ayu/data/ayu_database.h"
#include "ayu/features/filters/filters_controller.h"
#include "data/data_changes.h"
#include "data/data_groups.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "main/main_session.h"
#include "rpl/event_stream.h"

#include <list>
#include <memory>
#include <mutex>
#include <unordered_set>

namespace FiltersCacheController {

std::mutex rebuildMutex;
std::mutex cacheMutex;
std::mutex filteredMessagesMutex;

rpl::event_stream<> filtersUpdateStream;

void fireUpdate() {
	filtersUpdateStream.fire({});
}

rpl::producer<> updates() {
	return filtersUpdateStream.events();
}

std::shared_ptr<const Cache> cache;

namespace {

constexpr auto kMaxResultsPerSession = 8192;

using ResultId = std::pair<BareId, int64>;
using ResultOrder = std::list<ResultId>;

struct Result {
	ResultOrder::iterator position;
	bool filtered = false;
	bool blocked = false;
};

struct DialogResults {
	std::unordered_map<int64, Result> messages;
	int filteredCount = 0;
};

struct SessionResults {
	std::unordered_map<BareId, DialogResults> dialogs;
	ResultOrder order;
};

std::unordered_map<Main::Session*, SessionResults> filteredMessages;
std::unordered_set<Main::Session*> trackedSessions;

void EraseResult(SessionResults &session, BareId peer, int64 id) {
	const auto dialog = session.dialogs.find(peer);
	if (dialog == session.dialogs.end()) {
		return;
	}
	const auto entry = dialog->second.messages.find(id);
	if (entry == dialog->second.messages.end()) {
		return;
	}
	if (entry->second.filtered) {
		--dialog->second.filteredCount;
	}
	session.order.erase(entry->second.position);
	dialog->second.messages.erase(entry);
	if (dialog->second.messages.empty()) {
		session.dialogs.erase(dialog);
	}
}

void EraseHistory(not_null<Main::Session*> session, BareId peer) {
	std::lock_guard lock(filteredMessagesMutex);
	const auto account = filteredMessages.find(session);
	if (account == filteredMessages.end()) {
		return;
	}
	const auto dialog = account->second.dialogs.find(peer);
	if (dialog == account->second.dialogs.end()) {
		return;
	}
	for (const auto &[id, result] : dialog->second.messages) {
		account->second.order.erase(result.position);
	}
	account->second.dialogs.erase(dialog);
}

void ClearResults(not_null<Main::Session*> session) {
	std::lock_guard lock(filteredMessagesMutex);
	filteredMessages.erase(session);
}

void TrackSession(not_null<Main::Session*> session) {
	{
		std::lock_guard lock(filteredMessagesMutex);
		if (!trackedSessions.insert(session).second) {
			return;
		}
	}
	session->data().itemRemoved(
	) | rpl::on_next([](not_null<const HistoryItem*> item) {
		invalidate(item);
	}, session->lifetime());
	session->data().itemIdChanged(
	) | rpl::on_next([=](const Data::Session::IdChange &change) {
		std::lock_guard lock(filteredMessagesMutex);
		const auto account = filteredMessages.find(session);
		if (account != filteredMessages.end()) {
			EraseResult(account->second, change.newId.peer.value, change.oldId.bare);
			EraseResult(account->second, change.newId.peer.value, change.newId.msg.bare);
		}
	}, session->lifetime());
	session->data().itemDataChanges(
	) | rpl::on_next([](not_null<HistoryItem*> item) {
		invalidate(item);
	}, session->lifetime());
	session->data().historyUnloaded(
	) | rpl::on_next([=](not_null<const History*> history) {
		EraseHistory(session, history->peer->id.value);
	}, session->lifetime());
	session->data().historyCleared(
	) | rpl::on_next([=](not_null<const History*> history) {
		EraseHistory(session, history->peer->id.value);
	}, session->lifetime());
	session->data().sessionDataAboutToBeCleared(
	) | rpl::on_next([=] {
		ClearResults(session);
	}, session->lifetime());
	session->changes().peerUpdates(
		Data::PeerUpdate::Flag::IsBlocked
	) | rpl::on_next([=] {
		ClearResults(session);
	}, session->lifetime());
	const auto &settings = AyuSettings::getInstance();
	settings.filtersEnabledChanges(
	) | rpl::on_next([=] {
		ClearResults(session);
	}, session->lifetime());
	settings.hideFromBlockedChanges(
	) | rpl::on_next([=] {
		ClearResults(session);
	}, session->lifetime());
	settings.filtersEnabledInChatsChanges(
	) | rpl::on_next([=] {
		ClearResults(session);
	}, session->lifetime());
	session->lifetime().add([session] {
		std::lock_guard lock(filteredMessagesMutex);
		filteredMessages.erase(session);
		trackedSessions.erase(session);
	});
}

void PutResult(not_null<HistoryItem*> item, bool filtered, bool blocked) {
	auto &session = filteredMessages[&item->history()->session()];
	const auto peer = item->history()->peer->id.value;
	const auto id = item->id.bare;
	const auto existing = session.dialogs.find(peer);
	if (existing != session.dialogs.end()) {
		const auto entry = existing->second.messages.find(id);
		if (entry != existing->second.messages.end()) {
			existing->second.filteredCount += int(filtered)
				- int(entry->second.filtered);
			entry->second.filtered = filtered;
			entry->second.blocked = blocked;
			session.order.splice(
				session.order.end(),
				session.order,
				entry->second.position);
			return;
		}
	}
	while (session.order.size() >= kMaxResultsPerSession) {
		const auto oldest = session.order.front();
		EraseResult(session, oldest.first, oldest.second);
	}
	session.order.emplace_back(peer, id);
	auto &dialog = session.dialogs[peer];
	dialog.messages.emplace(id, Result{
		.position = std::prev(session.order.end()),
		.filtered = filtered,
		.blocked = blocked,
	});
	if (filtered) {
		++dialog.filteredCount;
	}
}

} // namespace

std::shared_ptr<const Cache> buildCache() {
	const auto filters = AyuDatabase::getAllRegexFilters();
	const auto exclusions = AyuDatabase::getAllFiltersExclusions();

	std::vector<HashablePattern> shared;
	std::unordered_map<ID, std::vector<ReversiblePattern>> byDialogId;

	for (const auto &filter : filters) {
		if (!filter.enabled || filter.text.empty()) {
			continue;
		}

		int flags = UREGEX_MULTILINE;
		if (filter.caseInsensitive) flags |= UREGEX_CASE_INSENSITIVE;

		auto status = U_ZERO_ERROR;
		auto pattern = icu::RegexPattern::compile(icu::UnicodeString::fromUTF8(filter.text), flags, status);

		if (U_FAILURE(status) || !pattern) {
			delete pattern;
			continue;
		}

		if (filter.dialogId.has_value()) {
			byDialogId[filter.dialogId.value()].push_back({
				std::shared_ptr<icu::RegexPattern>(pattern),
				filter.reversed,
			});
		} else {
			shared.push_back({
				filter.id,
				{
					std::shared_ptr<icu::RegexPattern>(pattern),
					filter.reversed,
				},
			});
		}
	}

	auto exclByDialogId = buildExclusions(exclusions, shared);
	auto result = std::make_shared<Cache>();
	result->sharedPatterns = std::move(shared);
	result->patternsByDialogId = std::move(byDialogId);
	result->exclusionsByDialogId = std::move(exclByDialogId);

	return result;
}

void rebuildCache() {
	std::lock_guard rebuildLock(rebuildMutex);
	auto next = buildCache();
	{
		std::lock_guard cacheLock(cacheMutex);
		std::lock_guard filteredLock(filteredMessagesMutex);
		cache = std::move(next);
		filteredMessages.clear();
	}
}

std::unordered_map<long long, std::unordered_set<HashablePattern, PatternHasher>> buildExclusions(
	const std::vector<RegexFilterGlobalExclusion> &exclusions,
	const std::vector<HashablePattern> &shared) {
	std::unordered_map<long long, std::unordered_set<HashablePattern, PatternHasher>> exclusionsByDialogId;

	for (const auto &exclusion : exclusions) {
		auto &exclusionSet = exclusionsByDialogId[exclusion.dialogId];

		for (const auto &filter : shared) {
			if (filter.id == exclusion.filterId) {
				exclusionSet.insert(filter);
				break;
			}
		}
	}
	return exclusionsByDialogId;
}

std::shared_ptr<const Cache> snapshot() {
	{
		std::lock_guard lock(cacheMutex);
		if (cache) {
			return cache;
		}
	}

	std::lock_guard rebuildLock(rebuildMutex);
	{
		std::lock_guard lock(cacheMutex);
		if (cache) {
			return cache;
		}
	}

	auto next = buildCache();
	std::lock_guard lock(cacheMutex);
	if (!cache) {
		cache = std::move(next);
	}
	return cache;
}

std::optional<bool> isFiltered(not_null<HistoryItem*> item) {
	const auto session = &item->history()->session();
	TrackSession(session);
	std::lock_guard lock(filteredMessagesMutex);
	const auto account = filteredMessages.find(session);
	if (account == filteredMessages.end()) {
		return std::nullopt;
	}
	const auto peer = item->history()->peer->id.value;
	const auto dialog = account->second.dialogs.find(peer);
	if (dialog == account->second.dialogs.end()) {
		return std::nullopt;
	}
	const auto entry = dialog->second.messages.find(item->id.bare);
	if (entry == dialog->second.messages.end()) {
		return std::nullopt;
	}
	if (entry->second.blocked) {
		EraseResult(account->second, peer, item->id.bare);
		return std::nullopt;
	}
	account->second.order.splice(
		account->second.order.end(),
		account->second.order,
		entry->second.position);
	return entry->second.filtered;
}

bool hasFilteredMessages(not_null<PeerData*> peer) {
	std::lock_guard lock(filteredMessagesMutex);
	const auto account = filteredMessages.find(&peer->session());
	if (account == filteredMessages.end()) {
		return false;
	}
	const auto dialog = account->second.dialogs.find(peer->id.value);
	return (dialog != account->second.dialogs.end())
		&& (dialog->second.filteredCount > 0);
}

void putHiddenBlockedMessage(not_null<HistoryItem*> item) {
	TrackSession(&item->history()->session());
	std::lock_guard lock(filteredMessagesMutex);
	PutResult(item, true, true);
}

void putFiltered(
		not_null<HistoryItem*> item,
		const Data::Group *group,
		bool res,
		const std::shared_ptr<const Cache> &matchedCache) {
	TrackSession(&item->history()->session());
	std::lock_guard cacheLock(cacheMutex);
	if (cache != matchedCache) {
		return;
	}

	std::lock_guard filteredLock(filteredMessagesMutex);
	PutResult(item, res, false);
	if (group && res) {
		for (const auto &groupItem : group->items) {
			PutResult(groupItem, true, false);
		}
	}
}

void invalidate(not_null<const HistoryItem*> item) {
	std::lock_guard lock(filteredMessagesMutex);
	const auto account = filteredMessages.find(&item->history()->session());
	if (account == filteredMessages.end()) {
		return;
	}
	if (const auto group = item->history()->owner().groups().find(item)) {
		for (const auto &groupItem : group->items) {
			EraseResult(
				account->second,
				groupItem->history()->peer->id.value,
				groupItem->id.bare);
		}
	} else {
		EraseResult(
			account->second,
			item->history()->peer->id.value,
			item->id.bare);
	}
}

}
