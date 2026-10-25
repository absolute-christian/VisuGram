// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026

#include "ayu/features/filters/filters_controller.h"

#include "ayu/ayu_settings.h"
#include "ayu/features/filters/filters_cache_controller.h"
#include "ayu/features/filters/filters_utils.h"
#include "ayu/utils/telegram_helpers.h"
#include "data/data_peer.h"
#include "data/data_peer_id.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_item_components.h"
#include "main/main_session.h"
#include "unicode/regex.h"

#include <crl/crl_time.h>

#include <map>
#include <memory>
#include <unordered_set>

namespace FiltersController {

namespace {

constexpr auto kMaxFilterTextLength = 64 * 1024;
constexpr auto kRegexTimeLimit = 5;
constexpr auto kRegexStackLimit = 256 * 1024;
constexpr auto kFilterBudget = crl::time(8);

std::map<Main::Session*, std::unordered_set<BareId>> showingFilteredMessages;

bool ShowingFilteredMessages(not_null<PeerData*> peer) {
	const auto session = showingFilteredMessages.find(&peer->session());
	return (session != showingFilteredMessages.end())
		&& session->second.contains(peer->id.value);
}

UBool U_CALLCONV ContinueMatching(const void *context, int32_t) {
	return crl::now() < *static_cast<const crl::time*>(context);
}

} // namespace

bool filterBlocked(const not_null<HistoryItem*> item) {
	if (item->from() != item->history()->peer) {
		if (isBlocked(item)) {
			return true;
		}
	}
	if (const auto bot = item->viaBot()) {
		if (isBlocked(bot)) {
			return true;
		}
	}
	return false;
}

std::optional<bool> isFiltered(
		const QString &str,
		long long dialogId,
		const std::shared_ptr<const FiltersCacheController::Cache> &cache) {
	if (str.isEmpty()) {
		return std::nullopt;
	} else if (str.size() > kMaxFilterTextLength) {
		return false;
	}

	const auto deadline = crl::now() + kFilterBudget;
	const auto icuStr = icu::UnicodeString(
		reinterpret_cast<const UChar*>(str.constData()),
		str.length());
	const auto matches = [&](const ReversiblePattern &pattern) {
		if (crl::now() >= deadline) {
			return false;
		}
		auto status = U_ZERO_ERROR;
		const auto matcher = std::unique_ptr<icu::RegexMatcher>(
			pattern.pattern->matcher(icuStr, status));
		if (U_FAILURE(status) || !matcher) {
			return false;
		}
		matcher->setTimeLimit(kRegexTimeLimit, status);
		matcher->setStackLimit(kRegexStackLimit, status);
		matcher->setMatchCallback(ContinueMatching, &deadline, status);
		const auto match = matcher->find(status);
		return U_SUCCESS(status) && (pattern.reversed ? !match : match);
	};

	if (const auto i = cache->patternsByDialogId.find(dialogId); i != cache->patternsByDialogId.end()) {
		for (const auto &pattern : i->second) {
			if (crl::now() >= deadline) {
				return false;
			}
			if (matches(pattern)) {
				return true;
			}
		}
	}

	const auto exclusions = cache->exclusionsByDialogId.find(dialogId);
	if (!cache->sharedPatterns.empty()) {
		for (const auto &pattern : cache->sharedPatterns) {
			if (crl::now() >= deadline) {
				return false;
			}
			if (exclusions != cache->exclusionsByDialogId.end() && exclusions->second.contains(pattern)) {
				continue;
			}
			if (matches(pattern.pattern)) {
				return true;
			}
		}
	}
	return false;
}

bool isEnabled(not_null<PeerData*> peer) {
	const auto &settings = AyuSettings::getInstance();
	return settings.filtersEnabled() && (settings.filtersEnabledInChats() || peer->isBroadcast());
}

bool isBlocked(const not_null<HistoryItem*> item) {
	const auto &settings = AyuSettings::getInstance();
	if (!settings.filtersEnabled()) {
		return false;
	}

	auto shadowBanMatched = false;
	const auto blocked = [&]() -> bool
	{
		const auto isShadowBanned = [&](PeerData *peer) {
			return peer
				&& (peer->isUser() || peer->isBroadcast())
				&& settings.isShadowBanned(getDialogIdFromPeer(peer));
		};

		if (isShadowBanned(item->from())
			&& item->from()->id != item->history()->peer->id) {
			shadowBanMatched = true;
			return true;
		}

		if (item->from()->isUser()
			&& item->from()->asUser()->isBlocked()) {
			// don't hide messages if it's a dialog with blocked user
			return item->from()->asUser()->id != item->history()->peer->id;
		}

		if (const auto forwarded = item->Get<HistoryMessageForwarded>()) {
			if (const auto originalSender = forwarded->originalSender) {
				const auto originalShadowBanned = isShadowBanned(originalSender);
				if (originalShadowBanned
					|| (originalSender->isUser()
						&& originalSender->asUser()->isBlocked())) {
					shadowBanMatched = originalShadowBanned;
					return true;
				}
			}
		}
		return false;
	}();

	return (shadowBanMatched || settings.hideFromBlocked()) && blocked;
}

bool isBlocked(const not_null<PeerData*> peer) {
	const auto &settings = AyuSettings::getInstance();
	return settings.filtersEnabled() &&
	(
		(peer->isUser() && peer->asUser()->isBlocked() && settings.hideFromBlocked()) ||
		((peer->isUser() || peer->isBroadcast()) && settings.isShadowBanned(getDialogIdFromPeer(peer)))
	);
}

bool filtered(const not_null<HistoryItem*> item) {
	const auto &settings = AyuSettings::getInstance();
	if (!settings.filtersEnabled()) {
		return false;
	}
	if (ShowingFilteredMessages(item->history()->peer)) {
		return false;
	}

	if (item->out()) {
		return false;
	}

	if (filterBlocked(item)) {
		FiltersCacheController::putHiddenBlockedMessage(item);
		return true;
	}

	if (!isEnabled(item->history()->peer)) return false;

	const auto cached = FiltersCacheController::isFiltered(item);
	if (cached.has_value()) {
		return *cached;
	}
	const auto cache = FiltersCacheController::snapshot();
	const auto dialogId = getDialogIdFromPeer(item->history()->peer);
	const auto local = cache->patternsByDialogId.find(dialogId);
	const auto exclusions = cache->exclusionsByDialogId.find(dialogId);
	const auto hasShared = ranges::any_of(cache->sharedPatterns, [&](
			const auto &pattern) {
		return (exclusions == cache->exclusionsByDialogId.end())
			|| !exclusions->second.contains(pattern);
	});
	if ((local == cache->patternsByDialogId.end() || local->second.empty())
		&& !hasShared) {
		return false;
	}
	const auto group = item->history()->owner().groups().find(item);
	const auto res = isFiltered(
		FilterUtils::extractAllText(item, group),
		dialogId,
		cache);

	// sometimes item has empty text.
	// so we cache result only if
	// processed item is filterable
	if (res.has_value()) {
		FiltersCacheController::putFiltered(item, group, *res, cache);
		return *res;
	}
	return false;
}

std::optional<bool> filteredMessagesShown(not_null<PeerData*> peer) {
	if (!AyuSettings::getInstance().filtersEnabled()) {
		return std::nullopt;
	}
	if (!ShowingFilteredMessages(peer)
		&& !FiltersCacheController::hasFilteredMessages(peer)) {
		return std::nullopt;
	}
	return ShowingFilteredMessages(peer);
}

void toggleFilteredMessagesShown(not_null<PeerData*> peer) {
	const auto session = &peer->session();
	const auto [entry, added] = showingFilteredMessages.try_emplace(session);
	if (added) {
		session->lifetime().add([session] {
			showingFilteredMessages.erase(session);
		});
	}
	auto &shown = entry->second;
	if (shown.contains(peer->id.value)) {
		shown.erase(peer->id.value);
	} else {
		shown.insert(peer->id.value);
	}
	FiltersCacheController::fireUpdate();
}

void invalidate(not_null<HistoryItem*> item) {
	const auto &settings = AyuSettings::getInstance();
	if (!settings.filtersEnabled()) {
		return;
	}

	FiltersCacheController::invalidate(item);
}

}
