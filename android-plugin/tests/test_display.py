import datetime
import sys
import types
import unittest
from unittest.mock import patch

from test_profile import plugin, USERS, UI, NETWORK, drain_ui

class List:
    def __init__(self, values=()):
        self.values = list(values)

    def size(self):
        return len(self.values)

    def get(self, index):
        return self.values[index]

    def add(self, *args):
        if len(args) == 1:
            self.values.append(args[0])
        else:
            self.values.insert(*args)

    def remove(self, value):
        if isinstance(value, int):
            self.values.pop(value)
        elif value in self.values:
            self.values.remove(value)

    def clear(self):
        self.values.clear()

    def isEmpty(self):
        return not self.values

    def contains(self, value):
        return value in self.values

class Map(dict):
    def put(self, key, value):
        self[key] = value

    def remove(self, key):
        self.pop(key, None)

class DTO:
    def __init__(self, **values):
        self.flags = 0
        self.sticker, self.owner_id, self.owner_address, self.gift_address = None, None, None, None
        self.from_id, self.peer, self.to_id, self.resale_amount, self.message = None, None, None, None, None
        self.upgrade_stars, self.limited, self.pinned_to_top = 0, False, False
        self.title, self.num, self.attributes, self.resell_amount = "Pepe", 33, List(), List([100])
        self.__dict__.update(values)

    def getClass(self):
        return types.SimpleNamespace(getSimpleName=lambda: type(self).__name__)

    def equals(self, value):
        return self is value

    def getDocument(self):
        return "model-document"

def dto(name):
    return type(name, (DTO,), {})

class Message:
    TYPE_DATE, TYPE_GIFT_STARS = 10, 30

    def __init__(self, account, owner, generate=True, media=False):
        self.messageOwner, self.currentAccount = owner, account
        self.dateKeyInt = owner.date // 86400
        self.dateKey, self.isDateObject = str(self.dateKeyInt), False
        self.messageText = "native-price-header"

    def getId(self):
        return self.messageOwner.id

class Calendar:
    HOUR_OF_DAY, MINUTE, SECOND, MILLISECOND = range(4)

    @staticmethod
    def getInstance():
        return Calendar()

    def setTimeInMillis(self, value):
        self.time = value

    def set(self, field, value):
        self.time = self.time // 86400000 * 86400000

    def getTimeInMillis(self):
        return self.time

def record(number="gift-1", **values):
    result = dict(id=number, sender_id="202", recipient_id="101", source="tl", slug="Pepe-33",
                  message="hello", message_data="", price="500", currency="XTR", date=1700000500,
                  active=True, hidden=False, pinned=False, anonymous=False)
    result.update(values)
    return result

def state(records=(), **values):
    result = dict(records=list(records), objects={}, count=len(records), cursor="gifts-v1:1", next="",
                  time=10**20, busy=None)
    result.update(values)
    return result

def gift_list(gifts=(), **values):
    result = DTO(currentAccount=0, dialogId=101, gifts=List(gifts), totalCount=len(gifts), loading=False,
                 endReached=True, isCollection=False, craftingGiftId=0, sort_by_date=True,
                 notifyUpdate=lambda: None, hasFilters=lambda: False)
    for name in ("unique", "unlimited", "limited", "upgradable", "hidden", "displayed"):
        setattr(result, "isInclude_" + name, lambda: True)
    result.__dict__.update(values)
    return result

def param(target, *args):
    result = types.SimpleNamespace(thisObject=target, args=list(args), results=[])
    result.setResult = result.results.append
    return result

class DisplayTests(unittest.TestCase):
    def setUp(self):
        USERS.clear()
        USERS[0] = types.SimpleNamespace(id=101, username="real")
        UI.clear()
        NETWORK.clear()
        self.subject = plugin.VisuGramPlugin()
        self.subject._loaded, self.subject._foreground = True, True
        self.subject.settings["sync_enabled"] = True
        self.subject._display_profile, self.subject._display_chat = True, True
        self.key = (0, "101", "received", "101")
        self.stars = types.SimpleNamespace(**{name: dto(name) for name in (
            "TL_savedStarGift", "starGiftAttributeOriginalDetails", "TL_starsAmount", "TL_starsTonAmount")})
        tlrpc = types.SimpleNamespace(**{name: dto(name) for name in (
            "TL_peerUser", "TL_textWithEntities", "TL_messageActionStarGift", "TL_messageActionStarGiftUnique",
            "TL_messageService", "TL_message")})
        java = types.ModuleType("java.lang")
        java.System = types.SimpleNamespace(identityHashCode=id)
        reference = types.ModuleType("java.lang.ref")
        reference.WeakReference = lambda value: types.SimpleNamespace(get=lambda: value)
        util = types.ModuleType("java.util")
        util.ArrayList, util.Calendar, util.Locale = List, Calendar, plugin.Locale
        tgnet = types.ModuleType("org.telegram.tgnet")
        tgnet.TLRPC = tlrpc
        tl = types.ModuleType("org.telegram.tgnet.tl")
        tl.TL_stars = self.stars
        messenger = types.ModuleType("org.telegram.messenger")
        messenger.MessageObject = Message
        messenger.LocaleController = types.SimpleNamespace(formatDateChat=lambda date: "date " + str(date))
        messenger.NotificationCenter = types.SimpleNamespace(starUserGiftsLoaded=17, messagesDidLoad=18)
        self.modules = patch.dict(sys.modules, {"java.lang": java, "java.lang.ref": reference, "java.util": util,
            "org.telegram.tgnet": tgnet, "org.telegram.tgnet.tl": tl, "org.telegram.messenger": messenger})
        self.modules.start()
        self.native = patch.object(plugin, "decode_gift", side_effect=lambda source: dto("TL_starGiftUnique")())
        self.native.start()

    def tearDown(self):
        self.native.stop()
        self.modules.stop()
        UI.clear()
        NETWORK.clear()

    def chat(self, real=(), dialog=202):
        fragment = DTO(messages=List(real), messagesDict=[Map({value.getId(): value for value in real})],
                       messagesByDays=Map(), messagesByDaysSorted=Map(), chatMode=0, currentEncryptedChat=None,
                       firstLoading=False, getCurrentAccount=lambda: 0, getDialogId=lambda: dialog,
                       isThreadChat=lambda: False)
        fragment.getClass = lambda: types.SimpleNamespace(getName=lambda: "org.telegram.ui.ChatActivity")
        fragment.chatAdapter = types.SimpleNamespace(updateRowsSafe=lambda: None, notifyDataSetChanged=lambda: None)
        for value in real:
            fragment.messagesByDays.setdefault(value.dateKey, List()).add(value)
        return fragment

    def test_ids_and_dates_are_stable_across_plugin_restarts(self):
        gift = record()
        self.assertLess(plugin.visual_message_id(gift), -0x3fffffff)
        self.assertEqual(plugin.visual_message_id(gift), plugin.visual_message_id(dict(gift)))
        self.assertNotEqual(plugin.visual_message_id(gift), plugin.visual_message_id(record("different")))
        saved = plugin.native_saved_gift(gift, "visual")
        self.assertEqual(saved.date, gift["date"])

    def test_saved_gift_replaces_real_provenance_preserves_comment_and_removes_sale(self):
        gift = dto("TL_starGiftUnique")()
        gift.attributes = List([dto("starGiftAttributeOriginalDetails")(), dto("starGiftAttributeBackdrop")()])
        rich = DTO(text="custom emoji", entities=List([DTO(document_id=99999999999999999, offset=0, length=2)]))
        with patch.object(plugin, "decode_gift", return_value=gift), patch.object(plugin, "gift_comment", return_value=rich):
            saved = plugin.native_saved_gift(record(pinned=True, hidden=True), "visual")
        self.assertIs(saved.message, rich)
        self.assertIs(gift.attributes.get(1).message, rich)
        self.assertEqual(gift.attributes.get(1).sender_id.user_id, 202)
        self.assertEqual(gift.attributes.get(1).recipient_id.user_id, 101)
        self.assertEqual(gift.resell_amount.size(), 0)
        self.assertIsNone(gift.owner_id)
        self.assertTrue(saved.pinned_to_top and saved.unsaved)

    def test_anonymous_received_gift_does_not_embed_sender(self):
        saved = plugin.native_saved_gift(record(sender_id="0", anonymous=True), "visual")
        self.assertIsNone(saved.from_id)
        self.assertIsNone(getattr(saved.gift.attributes.get(0), "sender_id", None))
        self.assertEqual(plugin.gift_dialog(record(sender_id="0"), "101"), "101")

    def test_native_unique_price_header_and_comment_have_renderable_card(self):
        gift = record(currency="TON", price="7500000000")
        saved = plugin.native_saved_gift(gift, "visual")
        result = plugin.native_gift_message(0, "101", gift, saved)
        self.assertEqual(result.type, Message.TYPE_GIFT_STARS)
        self.assertEqual(result.messageText, "native-price-header")
        self.assertEqual(result.messageOwner.action.gift.sticker, "model-document")
        self.assertIs(result.messageOwner.action.message, saved.message)
        self.assertEqual(result.messageOwner.date, gift["date"])
        self.assertFalse(result.messageOwner.out)

    def test_grid_adds_gifts_without_changing_real_count_or_pagination(self):
        real = DTO(msg_id=55, date=1700000000)
        target = gift_list([real], endReached=False)
        self.subject._display_pages[self.key] = state([record()], count=52, next="12")
        self.subject._merge_gift_list(target)
        self.assertEqual(target.totalCount, 53)
        self.assertEqual(target.gifts.size(), 2)
        self.subject._merge_gift_list(target)
        self.assertEqual(target.gifts.size(), 2)
        self.subject._restore_gift_list(self.subject._list_binding(target))
        self.assertEqual(target.totalCount, 1)
        self.assertEqual(target.gifts.values, [real])
        self.assertFalse(target.endReached)

    def test_grid_virtual_pages_continue_after_real_list_ends(self):
        target = gift_list()
        self.subject._display_pages[self.key] = state([record()], count=7, next="6")
        self.subject._merge_gift_list(target)
        p = param(target)
        self.subject._display_hook("profile_load", p, True)
        self.subject._display_hook("profile_load", p, False)
        self.assertEqual(p.results, [None])
        self.assertEqual(len(NETWORK), 1)
        self.assertEqual(target.gifts.size(), 1)
        self.assertFalse(target.endReached)

    def test_native_loading_is_not_used_to_fetch_catalog(self):
        target = gift_list(loading=True)
        self.subject._display_pages[self.key] = state([record()])
        with patch.object(self.subject, "_saved_display") as decode:
            self.subject._merge_gift_list(target)
            decode.assert_not_called()
        self.assertEqual(target.gifts.size(), 0)

    def test_background_native_gift_list_does_not_start_network_loading(self):
        with patch.object(plugin, "get_last_fragment", return_value=None):
            self.subject._merge_gift_list(gift_list())
        self.assertEqual(NETWORK, [])
        self.assertEqual(len(self.subject._display_pages), 0)

    def test_profile_filters_do_not_expose_hidden_or_reorder_real_value_sort(self):
        real = [DTO(msg_id=1, date=20), DTO(msg_id=2, date=30)]
        target = gift_list(real, sort_by_date=False)
        target.isInclude_hidden = lambda: False
        target.hasFilters = lambda: True
        self.subject._display_pages[self.key] = state([record(hidden=True)])
        self.subject._merge_gift_list(target)
        self.assertEqual(target.gifts.values, real)
        self.assertEqual(target.totalCount, 2)

    def test_collections_and_crafting_do_not_receive_unrelated_virtual_gifts(self):
        for values in ({"isCollection": True}, {"craftingGiftId": 42}):
            self.subject._merge_gift_list(gift_list(**values))
        self.assertEqual(NETWORK, [])

    def test_profile_uses_view_copy_and_restores_it(self):
        original = DTO(stargifts_count=2)
        layout = types.SimpleNamespace(setUserInfo=lambda info: None)
        fragment = DTO(userInfo=original, sharedMediaLayout=layout)
        self.subject._display_pages[self.key] = state([record()], count=52)
        with patch.object(self.subject, "_profile_context", return_value=(0, "101", "101")), \
             patch.object(plugin, "clone_user_info", side_effect=lambda value: DTO(stargifts_count=value.stargifts_count)):
            self.subject._overlay_profile(fragment)
            self.subject._overlay_profile(fragment)
        self.assertEqual(fragment.userInfo.stargifts_count, 54)
        self.assertEqual(original.stargifts_count, 2)
        self.subject._clear_display()
        self.assertIs(fragment.userInfo, original)

    def test_chat_reopens_without_duplicates_and_keeps_real_messages(self):
        real = Message(0, DTO(id=9, date=1700000400))
        fragment, key = self.chat([real]), (0, "101", "chat", "202")
        self.subject._display_pages[key] = state([record(), record("out", sender_id="101", recipient_id="202", date=1700000600)])
        self.subject._merge_chat(fragment, key)
        first_cards = fragment.messages.values[:]
        self.subject._merge_chat(fragment, key)
        self.assertEqual(fragment.messages.values, first_cards)
        cards = [value for value in fragment.messages.values if not value.isDateObject]
        self.assertEqual(len(cards), 3)
        self.assertIs(cards[-1], real)
        self.assertEqual([value.messageOwner.date for value in cards], [1700000600, 1700000500, 1700000400])
        self.assertTrue(cards[0].messageOwner.out)
        self.subject._clear_display()
        self.assertEqual(fragment.messages.values, [real])
        self.assertEqual(fragment.messagesDict[0], {9: real})

    def test_empty_chat_has_one_date_header_per_day_and_cleans_all(self):
        fragment, key = self.chat(), (0, "101", "chat", "202")
        self.subject._display_pages[key] = state([record(), record("older", date=1700000100)])
        self.subject._merge_chat(fragment, key)
        self.subject._merge_chat(fragment, key)
        self.assertEqual(sum(value.isDateObject for value in fragment.messages.values), 1)
        self.assertTrue(fragment.messages.values[-1].isDateObject)
        self.subject._clear_display()
        self.assertEqual(fragment.messages.values, [])
        self.assertEqual(fragment.messagesByDays, {})

    def test_cleanup_keeps_date_header_needed_by_new_real_message(self):
        fragment, key = self.chat(), (0, "101", "chat", "202")
        self.subject._display_pages[key] = state([record()])
        self.subject._merge_chat(fragment, key)
        real = Message(0, DTO(id=99, date=1700000501))
        fragment.messages.add(0, real)
        fragment.messagesByDays.get(real.dateKey).add(real)
        fragment.messagesDict[0].put(99, real)
        self.subject._clear_display()
        self.assertIs(fragment.messages.values[0], real)
        self.assertTrue(fragment.messages.values[1].isDateObject)
        self.assertEqual(fragment.messagesByDays.get(real.dateKey).values, [real])

    def test_older_card_waits_for_native_history_and_threads_are_ignored(self):
        fragment, key = self.chat([Message(0, DTO(id=9, date=1700000700))]), (0, "101", "chat", "202")
        self.subject._display_pages[key] = state([record()])
        self.subject._merge_chat(fragment, key)
        self.assertEqual(len(fragment.messages.values), 1)
        fragment.chatMode = 2
        self.assertIsNone(self.subject._chat_context(fragment))
        fragment.chatMode = 0
        fragment.isThreadChat = lambda: True
        self.assertIsNone(self.subject._chat_context(fragment))

    def test_virtual_card_view_returns_sheet_for_native_show_chaining(self):
        self.subject._display_pages[self.key] = state([record()])
        saved = self.subject._saved_display(self.key, record())
        calls = []
        sheet = DTO(currentAccount=0, dialogId=202, set=lambda *args: calls.append(args))
        p = param(sheet, types.SimpleNamespace(getId=lambda: saved.msg_id))
        self.subject._display_hook("gift_sheet", p, True)
        self.assertEqual(p.results, [sheet])
        self.assertEqual(calls, [(saved, None)])
        self.assertEqual(sheet.dialogId, 0)

    def test_card_still_opens_when_another_views_registry_entry_is_evicted(self):
        key = 0, "101", "chat", "202"
        self.subject._display_pages[key] = state([record()])
        saved = self.subject._saved_display(key, record())
        self.subject._display_registry.clear()
        entry = self.subject._display_entry(0, saved.msg_id)
        self.assertEqual(entry[1]["id"], record()["id"])
        self.assertIs(entry[2], saved)

    def test_display_fetch_is_lazy_bounded_and_account_guarded(self):
        self.assertEqual(NETWORK, [])
        value = self.subject._display_page(self.key)
        self.subject._display_page(self.key)
        self.assertEqual(len(NETWORK), 1)
        with patch.object(plugin, "post_json", return_value={"gifts": [record()], "count": 1,
                          "cursor": "gifts-v1:1", "next_offset": ""}), patch.object(self.subject, "_display_visible"):
            NETWORK.pop(0)()
            USERS[0] = types.SimpleNamespace(id=404)
            drain_ui()
        self.assertEqual(value["records"], [])
        self.subject._toggle(False)
        self.assertEqual(len(self.subject._display_pages), 0)

    def test_malformed_oversized_or_wrong_owner_page_is_not_rendered(self):
        for gifts in ([record()] * 7, [record(recipient_id="303")]):
            self.subject._display_pages.clear()
            value = self.subject._display_page(self.key)
            with patch.object(plugin, "post_json", return_value={"gifts": gifts, "count": 7,
                              "cursor": "cursor", "next_offset": ""}):
                NETWORK.pop(0)()
                drain_ui()
            self.assertEqual(value["records"], [])

    def test_page_refresh_preserves_loaded_pages_if_cursor_did_not_change(self):
        records = [record(str(number)) for number in range(12)]
        value = state(records, busy=self.subject._generation, count=20, next="7")
        self.subject._display_pages[self.key] = value
        page = {"gifts": records[:6], "count": 20, "cursor": value["cursor"], "next_offset": "13"}
        with patch.object(self.subject, "_display_visible"):
            self.subject._display_finish(self.key, value, self.subject._generation, "", page, "")
        self.assertEqual(value["records"], records)
        self.assertEqual(value["next"], "7")

    def test_changed_cursor_replaces_old_cards_instead_of_appending_duplicates(self):
        value = state([record("old")], busy=self.subject._generation)
        self.subject._display_pages[self.key] = value
        page = {"gifts": [record("new")], "count": 1, "cursor": "new-cursor", "next_offset": ""}
        with patch.object(self.subject, "_display_visible"):
            self.subject._display_finish(self.key, value, self.subject._generation, "", page, "")
        self.assertEqual([gift["id"] for gift in value["records"]], ["new"])

    def test_real_reload_recreates_only_missing_virtual_messages(self):
        fragment, key = self.chat(), (0, "101", "chat", "202")
        self.subject._display_pages[key] = state([record()])
        self.subject._merge_chat(fragment, key)
        fragment.messages.clear()
        fragment.messagesDict[0].clear()
        fragment.messagesByDays.clear()
        fragment.messagesByDaysSorted.clear()
        self.subject._merge_chat(fragment, key)
        self.assertEqual(sum(not value.isDateObject for value in fragment.messages.values), 1)

    def test_account_switch_does_not_open_another_accounts_virtual_gift(self):
        self.subject._display_pages[self.key] = state([record()])
        saved = self.subject._saved_display(self.key, record())
        USERS[0] = types.SimpleNamespace(id=303)
        sheet = DTO(currentAccount=0, set=lambda *args: self.fail("opened gift for previous account"))
        p = param(sheet, types.SimpleNamespace(getId=lambda: saved.msg_id))
        self.subject._display_hook("gift_sheet", p, True)
        self.assertEqual(p.results, [])

    def test_view_cache_evicts_inactive_profiles_and_drops_native_objects(self):
        with patch.object(self.subject, "_request_display"):
            for number in range(plugin.DISPLAY_LIMIT + 2):
                self.subject._display_page((0, "101", "received", str(100 + number)))
        self.assertEqual(len(self.subject._display_pages), plugin.DISPLAY_LIMIT)
        self.assertNotIn((0, "101", "received", "100"), self.subject._display_pages)

if __name__ == "__main__":
    unittest.main()
