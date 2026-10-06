import base64
import struct
import sys
import types
import unittest
from unittest.mock import patch

from test_profile import plugin, USERS, UI, NETWORK, DELAYED, UserConfig, drain_ui

def named(class_name, **fields):
    return types.SimpleNamespace(getClass=lambda: types.SimpleNamespace(getSimpleName=lambda: class_name), **fields)

def values(items):
    return types.SimpleNamespace(size=lambda: len(items), get=lambda index: items[index])

def ordinary():
    return named("TL_starGift_layer219", id=15, sticker=object(), stars=25, convert_stars=20,
                 limited=False, sold_out=False, title="Rose")

def serialize(value):
    return plugin.tl_int(0x36f8c871) + struct.pack("<q", 7)

def record(**changes):
    result = {"id": "gift-id", "sender_id": "101", "recipient_id": "202", "source": "AAAAAA==",
              "slug": "", "message": "", "message_data": "", "price": "25", "currency": "XTR",
              "date": 1791210000, "active": True, "anonymous": False, "hidden": False, "pinned": False}
    return dict(result, **changes)

class GiftTests(unittest.TestCase):
    def setUp(self):
        UI.clear()
        NETWORK.clear()
        DELAYED.clear()
        USERS.clear()
        USERS[0] = types.SimpleNamespace(id=101, username="real")
        USERS[1] = types.SimpleNamespace(id=202, username="other")
        UserConfig.selectedAccount = 0
        self.subject = plugin.VisuGramPlugin()
        self.subject._loaded = self.subject._foreground = True
        self.subject.settings.update(sync_enabled=True, visual_gifts=True)
        self.callbacks = []
        self.subject._gift_callback = lambda callback, success: self.callbacks.append(success)

    def test_payment_firewall_is_independent_of_network_or_hook_availability(self):
        self.subject.settings["sync_enabled"] = False
        self.subject._foreground = False
        for name in plugin.PAYMENT_REQUESTS:
            response = self.subject.pre_request_hook(name, 0, object())
            self.assertEqual(response.strategy, "cancel")
        self.subject.settings["visual_gifts"] = False
        self.assertFalse(hasattr(self.subject.pre_request_hook(plugin.PAYMENT_REQUESTS[0], 0, object()), "strategy"))
        virtual = types.SimpleNamespace(stargift=types.SimpleNamespace(msg_id=-1))
        self.assertEqual(self.subject.pre_request_hook("saveStarGift", 0, virtual).strategy, "cancel")
        real = types.SimpleNamespace(stargift=types.SimpleNamespace(msg_id=27))
        self.assertFalse(hasattr(self.subject.pre_request_hook("saveStarGift", 0, real), "strategy"))

    def test_buy_hook_stops_real_method_before_encoding_failure(self):
        results = []
        param = types.SimpleNamespace(thisObject=types.SimpleNamespace(currentAccount=0),
                                      args=[ordinary(), False, False, 202, None, object()],
                                      setResult=lambda value: results.append(value))
        with patch.object(plugin, "gift_payload", side_effect=ValueError("bad format")):
            plugin.GiftBuyHook(self.subject).before_hooked_method(param)
        self.assertEqual(results, [None])
        self.assertEqual(self.callbacks, [False])
        self.assertEqual(NETWORK, [])

    def test_disabled_buy_hook_leaves_native_method_untouched(self):
        self.subject.settings["visual_gifts"] = False
        param = types.SimpleNamespace(setResult=lambda value: self.fail("intercepted real mode"))
        plugin.GiftBuyHook(self.subject).before_hooked_method(param)

    def test_buy_preserves_message_and_canonical_source(self):
        message = types.SimpleNamespace(text="🌟 hi")
        encoded = b"rich-emoji-data!!"
        payload = plugin.gift_payload(ordinary(), 202, message, True, 25, "XTR", lambda value: encoded if value is message else serialize(value))
        self.assertEqual(payload["message"], "🌟 hi")
        self.assertEqual(base64.b64decode(payload["message_data"]), encoded)
        self.assertEqual(payload["recipient_id"], "202")
        self.assertTrue(payload["anonymous"])
        source = base64.b64decode(payload["source"])
        self.assertEqual(struct.unpack_from("<IIq", source), (0x313a9547, 32, 15))
        self.assertEqual(struct.unpack_from("<qq", source, 28), (25, 20))
        self.assertEqual(source[44:], plugin.tl_string("Rose"))

    def test_old_collectible_attributes_are_translated_to_desktop_schema(self):
        model = named("starGiftAttributeModel", name="Void", document=object(), rarity_permille=15)
        backdrop = named("starGiftAttributeBackdrop", name="Black", backdrop_id=1,
                         center_color=2, edge_color=3, pattern_color=4, text_color=5, rarity_permille=20)
        gift = named("TL_starGiftUnique", id=1, title="Pepe", slug="PlushPepe-7", num=7,
                     attributes=values([model, backdrop]), availability_issued=100, availability_total=1000)
        data = base64.b64decode(plugin.gift_source(gift, serialize))
        self.assertEqual(struct.unpack_from("<IIqq", data), (0x85f0a9cd, 0, 1, 0))
        self.assertIn(plugin.tl_int(0x565251e2) + plugin.tl_int(0) + plugin.tl_string("Void"), data)
        self.assertIn(plugin.tl_int(0x36437737) + plugin.tl_int(15), data)
        self.assertIn(plugin.tl_int(0x9f2504e4) + plugin.tl_string("Black"), data)
        self.assertEqual(struct.unpack_from("<II", data, len(data) - 8), (100, 1000))

    def test_retry_uses_same_operation_and_registers_sender_before_send(self):
        payload = plugin.gift_payload(ordinary(), 202, None, False, 25, "XTR", serialize)
        requests = []

        def request(path, user_id, body, **kwargs):
            requests.append((path, user_id, dict(body)))
            if path == "/v1/gifts":
                raise OSError("offline")
            return {}

        with patch.object(plugin, "post_json", side_effect=request):
            for _ in range(2):
                self.subject._send_visual(0, payload, object())
                NETWORK.pop(0)()
                drain_ui()
        self.assertEqual([item[0] for item in requests], ["/v1/sync", "/v1/gifts"] * 2)
        self.assertEqual(requests[1][2]["operation_id"], requests[3][2]["operation_id"])
        self.assertEqual(self.callbacks, [False, False])

    def test_inflight_send_is_coalesced_and_logout_rejects_completion(self):
        payload = plugin.gift_payload(ordinary(), 202, None, False, 25, "XTR", serialize)
        self.subject._send_visual(0, payload, object())
        self.subject._send_visual(0, payload, object())
        self.assertEqual(len(NETWORK), 1)
        USERS[0] = None
        with patch.object(plugin, "post_json", side_effect=[{}, {"gift": record()}]):
            NETWORK.pop(0)()
            drain_ui()
        self.assertEqual(self.callbacks, [False, False])
        self.assertIsNone(self.subject._gift_page_state)
        self.assertEqual(self.subject._gift_busy, {})

    def test_local_only_has_clear_warning_and_bounded_account_cache(self):
        payload = plugin.gift_payload(ordinary(), 202, None, False, 25, "XTR", serialize)
        self.subject.settings["local_gifts:101"] = [record(id=str(i)) for i in range(20)]
        with patch.object(plugin, "post_json", side_effect=[{}, {"local_only": True}]), \
             patch.object(self.subject, "_show_error") as warning, patch.object(self.subject, "_refresh"):
            self.subject._send_visual(0, payload, object())
            NETWORK.pop(0)()
            drain_ui()
        warning.assert_called_once_with("LOCAL_ONLY")
        local = self.subject.settings["local_gifts:101"]
        self.assertEqual(len(local), 12)
        self.assertTrue(local[0]["local_only"])
        self.assertEqual(local[0]["recipient_id"], "202")
        self.assertNotIn("local_gifts:202", self.subject.settings)
        self.assertEqual(self.callbacks, [True])

    def test_page_coalescing_generation_and_account_guards(self):
        page = {"gifts": [record(recipient_id="101")], "count": 1, "cursor": "gifts-v1:1", "next_offset": ""}
        self.subject._load_gift_page()
        self.subject._load_gift_page()
        self.assertEqual(len(NETWORK), 1)
        with patch.object(plugin, "post_json", return_value=page) as request:
            NETWORK.pop(0)()
            drain_ui()
        self.assertEqual(request.call_args.args[0], "/v1/gifts/page")
        self.assertEqual(self.subject._gift_page_state["user_id"], "101")
        generation = self.subject._generation
        self.subject._generation += 1
        self.subject._gift_page_busy = self.subject._generation
        self.subject._page_finish(0, "101", generation, page, "")
        self.assertEqual(self.subject._gift_page_busy, self.subject._generation)
        fields = self.subject._gift_settings()
        self.assertTrue(any(getattr(item, "subtext", "") == "25 ★" for item in fields))
        UserConfig.selectedAccount = 1
        self.assertFalse(any(getattr(item, "subtext", "") == "25 ★" for item in self.subject._gift_settings()))

    def test_prices_and_invalid_records(self):
        self.assertEqual(plugin.gift_price(record(price="10000000000", currency="TON")), "10 TON")
        self.assertEqual(plugin.gift_price(record(price="139500000000", currency="TON")), "139.5 TON")
        self.assertEqual(plugin.gift_price(record(price="0", currency="TON")), "0 TON")
        for change in ({"price": "nan"}, {"date": -1}, {"source": 4}, {"recipient_id": "0"}):
            with self.subTest(change=change), self.assertRaises(ValueError):
                plugin.validate_gift(record(**change))

    def test_unsupported_hooks_cannot_enable_visual_purchases(self):
        self.subject._gift_native, self.subject._payment_guard = False, False
        self.subject._gift_toggle(True)
        self.assertFalse(self.subject.settings["visual_gifts"])

    def test_only_recipient_can_manage_gifts(self):
        with patch.object(self.subject, "_show_error") as error:
            self.subject._manage_visual(record(recipient_id="202", slug="PlushPepe-7"), {"pinned": True})
        error.assert_called_once_with("NOT_GIFT_OWNER")
        self.assertEqual(NETWORK, [])

    def test_manage_error_and_retry_keep_operation_id(self):
        requests = []

        def request(path, user_id, body):
            requests.append(dict(body))
            raise OSError("offline")

        with patch.object(plugin, "post_json", side_effect=request):
            for _ in range(2):
                self.subject._manage_visual(record(recipient_id="101", slug="PlushPepe-7"), {"pinned": True})
                NETWORK.pop(0)()
                drain_ui()
        self.assertEqual(requests[0]["operation_id"], requests[1]["operation_id"])
        self.assertEqual(requests[0]["id"], "gift-id")
        self.assertTrue(requests[0]["pinned"])

class Stream:
    def __init__(self, data):
        self.data, self.offset, self.closed, self.reported_position = data, 0, False, 0

    def readInt32(self, exception):
        value, = struct.unpack_from("<i", self.data, self.offset)
        self.offset += 4
        self.reported_position += 4
        return value

    def getPosition(self):
        return self.reported_position

    def remaining(self):
        return len(self.data) - self.offset

    def readInt64(self, exception):
        value, = struct.unpack_from("<q", self.data, self.offset)
        self.offset += 8
        self.reported_position += 8
        return value

    def readString(self, exception):
        start = self.offset
        length = self.data[start]
        prefix = 1
        if length == 254:
            length = int.from_bytes(self.data[start + 1:start + 4], "little")
            prefix = 4
        end = start + prefix + length
        self.offset = end + (-(prefix + length)) % 4
        self.reported_position += prefix + 1 + (-(prefix + length)) % 4
        return self.data[start + prefix:end].decode("utf-8")

    def cleanup(self):
        self.closed = True

class JavaList(list):
    def add(self, value):
        self.append(value)

    def size(self):
        return len(self)

    def get(self, index):
        return self[index]

def fake_module(name, **fields):
    result = types.ModuleType(name)
    result.__dict__.update(fields)
    return result

class GiftCodecTests(unittest.TestCase):
    def setUp(self):
        def read_document(stream, constructor, exception):
            self.assertEqual(constructor & 0xffffffff, 0x36f8c871)
            return types.SimpleNamespace(id=stream.readInt64(True))

        class OldGift:
            def __init__(self):
                self.attributes = JavaList()

            def getDocument(self):
                return self.sticker if hasattr(self, "sticker") else self.attributes[0].document

            def getClass(self):
                return types.SimpleNamespace(getSimpleName=lambda: "TL_starGiftUnique" if self.attributes else "TL_starGift")

        self.stars = types.SimpleNamespace(
            StarGift=types.SimpleNamespace(TLdeserialize=lambda stream, constructor, exception: None),
            TL_starGift=OldGift, TL_starGiftUnique=OldGift,
            starGiftAttributeModel=lambda: named("starGiftAttributeModel"),
            starGiftAttributePattern=lambda: named("starGiftAttributePattern"),
            starGiftAttributeBackdrop=lambda: named("starGiftAttributeBackdrop"),
            TL_savedStarGift=types.SimpleNamespace,
        )
        self.rpc = types.SimpleNamespace(Document=types.SimpleNamespace(TLdeserialize=read_document))
        self.modules = {
            "org.telegram.tgnet": fake_module("org.telegram.tgnet", TLRPC=self.rpc),
            "org.telegram.tgnet.tl": fake_module("org.telegram.tgnet.tl", TL_stars=self.stars),
        }
        self.streams = []

    def stream(self, encoded):
        stream = Stream(base64.b64decode(encoded))
        self.streams.append(stream)
        return stream

    def test_old_android_decodes_current_desktop_basic_gift(self):
        gift = ordinary()
        gift.limited, gift.sold_out = True, True
        gift.availability_remains, gift.availability_total = 0, 1000
        gift.first_sale_date, gift.last_sale_date = 100, 200
        source = plugin.gift_source(gift, serialize)
        with patch.dict(sys.modules, self.modules), patch.object(plugin, "native_stream", self.stream):
            decoded = plugin.decode_gift(source)
        self.assertEqual((decoded.id, decoded.stars, decoded.convert_stars, decoded.title), (15, 25, 20, "Rose"))
        self.assertEqual(decoded.availability_total, 1000)
        self.assertEqual(decoded.last_sale_date, 200)
        self.assertTrue(all(stream.closed for stream in self.streams))

    def test_android_string_position_does_not_reject_complete_native_gift(self):
        source = plugin.gift_source(ordinary(), serialize)
        gift = named("TL_starGift", getDocument=lambda: object())

        def read_native(stream, constructor, strict):
            self.assertEqual(constructor & 0xffffffff, 0x313a9547)
            stream.readInt32(True)
            stream.readInt64(True)
            self.rpc.Document.TLdeserialize(stream, stream.readInt32(True), True)
            stream.readInt64(True)
            stream.readInt64(True)
            gift.title = stream.readString(True)
            return gift

        self.stars.StarGift.TLdeserialize = read_native
        with patch.dict(sys.modules, self.modules), patch.object(plugin, "native_stream", self.stream):
            decoded = plugin.decode_gift(source)
        self.assertIs(decoded, gift)
        self.assertEqual(gift.title, "Rose")
        self.assertEqual(self.streams[0].remaining(), 0)
        self.assertNotEqual(self.streams[0].getPosition(), len(base64.b64decode(source)))

    def test_complete_native_gift_with_trailing_data_is_still_rejected(self):
        source = plugin.gift_source(ordinary(), serialize)
        source = base64.b64encode(base64.b64decode(source) + b"\x00" * 4).decode("ascii")

        def read_native(stream, constructor, strict):
            stream.readInt32(True)
            stream.readInt64(True)
            self.rpc.Document.TLdeserialize(stream, stream.readInt32(True), True)
            stream.readInt64(True)
            stream.readInt64(True)
            stream.readString(True)
            return named("TL_starGift", getDocument=lambda: object())

        self.stars.StarGift.TLdeserialize = read_native
        with patch.dict(sys.modules, self.modules), patch.object(plugin, "native_stream", self.stream):
            with self.assertRaises(plugin.ApiError):
                plugin.decode_gift(source)

    def test_collectible_reaches_shared_profile_preview_with_android_stream(self):
        self.rpc.TL_peerUser = types.SimpleNamespace
        self.stars.starGiftAttributeOriginalDetails = lambda: named("starGiftAttributeOriginalDetails", flags=0)
        subject = plugin.VisuGramPlugin()
        key = 0, "101", "received", "202"
        subject._display_pages[key] = {"objects": {}}
        value = record(source=self.collectible(), message="", message_data="")
        with patch.dict(sys.modules, self.modules), patch.object(plugin, "native_stream", self.stream):
            saved = subject._saved_display(key, value)
        self.assertEqual(saved.gift.slug, "PlushPepe-7")
        self.assertEqual(saved.gift.attributes[1].name, "Black")
        self.assertIsNone(saved.gift.resell_amount)
        self.assertEqual(self.streams[-1].remaining(), 0)
        self.assertNotEqual(self.streams[-1].getPosition(), len(base64.b64decode(value["source"])))

    def test_old_android_decodes_model_symbol_background_and_rarity(self):
        attributes = [
            named("starGiftAttributeModel", name="Void", document=object(), rarity_permille=15),
            named("starGiftAttributePattern", name="Crystal", document=object(), rarity_permille=8),
            named("starGiftAttributeBackdrop", name="Black", backdrop_id=1,
                  center_color=2, edge_color=3, pattern_color=4, text_color=5, rarity_permille=20),
        ]
        gift = named("TL_starGiftUnique", id=1, title="Pepe", slug="PlushPepe-7", num=7,
                     attributes=values(attributes), availability_issued=100, availability_total=1000)
        source = plugin.gift_source(gift, serialize)
        with patch.dict(sys.modules, self.modules), patch.object(plugin, "native_stream", self.stream):
            decoded = plugin.decode_gift(source)
        self.assertEqual(decoded.slug, "PlushPepe-7")
        self.assertEqual([attribute.name for attribute in decoded.attributes], ["Void", "Crystal", "Black"])
        self.assertEqual([attribute.rarity_permille for attribute in decoded.attributes], [15, 8, 20])
        self.assertEqual(decoded.attributes[2].pattern_color, 4)
        self.assertEqual(decoded.availability_total, 1000)

    def collectible(self, backdrop=True):
        attributes = [named("starGiftAttributeModel", name="Void", document=object(), rarity_permille=15)]
        if backdrop:
            attributes.append(named("starGiftAttributeBackdrop", name="Black", backdrop_id=1,
                                    center_color=2, edge_color=3, pattern_color=4, text_color=5, rarity_permille=20))
        gift = named("TL_starGiftUnique", id=1, title="Pepe", slug="PlushPepe-7", num=7,
                     attributes=values(attributes), availability_issued=100, availability_total=1000)
        return plugin.gift_source(gift, serialize)

    def test_partial_native_decode_does_not_reach_avatar_constructor(self):
        partial = named("TL_starGiftUnique", getDocument=lambda: object(), attributes=JavaList())
        native_calls = []

        def incomplete(stream, constructor, strict):
            native_calls.append(strict)
            stream.readInt32(True)
            return partial

        self.stars.StarGift.TLdeserialize = incomplete
        with patch.dict(sys.modules, self.modules), patch.object(plugin, "native_stream", self.stream):
            decoded = plugin.decode_gift(self.collectible())
        self.assertIsNot(decoded, partial)
        self.assertEqual(decoded.attributes[1].name, "Black")
        self.assertEqual(native_calls, [True])
        self.assertTrue(all(stream.closed for stream in self.streams))

    def test_collectible_without_backdrop_is_rejected_before_native_rendering(self):
        with patch.dict(sys.modules, self.modules), patch.object(plugin, "native_stream", self.stream):
            with self.assertRaises(plugin.ApiError):
                plugin.decode_gift(self.collectible(backdrop=False))

    def test_desktop_resale_tail_is_consumed_without_leaving_partial_gift(self):
        source = bytearray(base64.b64decode(self.collectible()))
        struct.pack_into("<I", source, 4, 8 | 16 | 256)
        source += plugin.tl_string("old-wallet") + struct.pack("<II", 0x1cb5c415, 1)
        source += struct.pack("<Iqi", 0xbbb6b4a3, 500, 0)
        source += struct.pack("<q", 123) + plugin.tl_string("USD") + struct.pack("<q", 456)
        self.stars.StarsAmount = types.SimpleNamespace(TLdeserialize=lambda stream, constructor, strict:
            types.SimpleNamespace(amount=stream.readInt64(True), nanos=stream.readInt32(True)))
        with patch.dict(sys.modules, self.modules), patch.object(plugin, "native_stream", self.stream):
            decoded = plugin.decode_gift(base64.b64encode(source).decode("ascii"))
        self.assertEqual(decoded.gift_address, "old-wallet")
        self.assertEqual(decoded.value_currency, "USD")
        self.assertEqual(self.streams[-1].offset, len(source))

    def test_trailing_gift_data_is_not_silently_accepted(self):
        encoded = base64.b64encode(base64.b64decode(self.collectible()) + b"\x00" * 4).decode("ascii")
        with patch.dict(sys.modules, self.modules), patch.object(plugin, "native_stream", self.stream):
            with self.assertRaises(plugin.ApiError):
                plugin.decode_gift(encoded)

    def test_preview_keeps_custom_emoji_and_uses_nonpayment_reference(self):
        subject = plugin.VisuGramPlugin()
        subject._loaded = subject._foreground = True
        subject.settings["sync_enabled"] = True
        captures = []

        class Sheet:
            def __init__(self, *args):
                self.args = args

            def set(self, saved, list):
                captures.append((self.args, saved))

            def show(self):
                pass

        def read_message(stream, constructor, exception):
            self.assertEqual(constructor & 0xffffffff, 0x751f3146)
            text = stream.readString(True)
            self.assertEqual(stream.readInt32(True) & 0xffffffff, 0x1cb5c415)
            self.assertEqual(stream.readInt32(True), 1)
            self.assertEqual(stream.readInt32(True) & 0xffffffff, 0xc8cf05f8)
            entity = types.SimpleNamespace(offset=stream.readInt32(True), length=stream.readInt32(True),
                                           document_id=stream.readInt64(True))
            return types.SimpleNamespace(text=text, entities=JavaList([entity]))

        message = plugin.tl_int(0x751f3146) + plugin.tl_string("👑 hi") + plugin.tl_int(0x1cb5c415) + plugin.tl_int(1)
        message += plugin.tl_int(0xc8cf05f8) + struct.pack("<iiq", 0, 2, 5368324170671202286)
        self.rpc.TL_peerUser = types.SimpleNamespace
        self.rpc.TL_textWithEntities = types.SimpleNamespace(TLdeserialize=read_message)
        self.modules["org.telegram.ui.Stars"] = fake_module("org.telegram.ui.Stars", StarGiftSheet=Sheet)
        fragment = types.SimpleNamespace(getParentActivity=lambda: object(), getResourceProvider=lambda: None,
                                         getCurrentAccount=lambda: 0)
        source = plugin.gift_source(ordinary(), serialize)
        value = record(source=source, message="👑 hi", message_data=base64.b64encode(message).decode("ascii"))
        with patch.dict(sys.modules, self.modules), patch.object(plugin, "native_stream", self.stream), \
             patch.object(plugin, "get_last_fragment", return_value=fragment):
            subject._open_gift(value)
        self.assertEqual(len(captures), 1)
        arguments, saved = captures[0]
        self.assertEqual(arguments[2], 0)
        self.assertEqual(saved.msg_id, plugin.visual_message_id(value))
        self.assertEqual(saved.message.text, "👑 hi")
        self.assertEqual(saved.message.entities[0].document_id, 5368324170671202286)
        self.assertEqual(saved.message.entities[0].length, 2)

class CommentCodecTests(unittest.TestCase):
    def setUp(self):
        self.streams, self.issues = [], []

        class Text:
            def __init__(self):
                self.text, self.entities = "", JavaList()

            @staticmethod
            def TLdeserialize(stream, constructor, exception):
                if constructor & 0xffffffff != 0x751f3146:
                    raise ValueError("unsupported text constructor")
                value = Text()
                value.text = stream.readString(True)
                if stream.readInt32(True) & 0xffffffff != 0x1cb5c415:
                    raise ValueError("invalid vector")
                for _ in range(stream.readInt32(True)):
                    if stream.readInt32(True) & 0xffffffff != 0xc8cf05f8:
                        raise ValueError("unsupported entity constructor")
                    value.entities.add(types.SimpleNamespace(offset=stream.readInt32(True), length=stream.readInt32(True),
                                                            document_id=stream.readInt64(True)))
                return value

        self.Text = Text
        self.rpc = types.SimpleNamespace(TL_textWithEntities=Text)
        self.module_patch = patch.dict(sys.modules, {"org.telegram.tgnet": fake_module("org.telegram.tgnet", TLRPC=self.rpc)})
        self.module_patch.start()
        self.stream_patch = patch.object(plugin, "native_stream", side_effect=self.stream)
        self.stream_patch.start()

    def tearDown(self):
        self.stream_patch.stop()
        self.module_patch.stop()

    def stream(self, encoded):
        value = Stream(base64.b64decode(encoded))
        self.streams.append(value)
        return value

    def comment(self, data, text="ку"):
        value = record(message=text, message_data=base64.b64encode(data).decode("ascii"))
        return plugin.gift_comment(value, lambda *args: self.issues.append(args))

    def test_exact_bare_desktop_comment_from_report_is_read(self):
        data = bytes.fromhex("04d0bad18300000015c4b51c00000000")
        self.assertEqual(struct.unpack_from("<I", data)[0], 0xd1bad004)
        with self.assertRaisesRegex(ValueError, "unsupported text constructor"):
            self.Text.TLdeserialize(Stream(data), 0xd1bad004, True)
        value = self.comment(data)
        self.assertEqual(value.text, "ку")
        self.assertEqual(value.entities, [])
        self.assertEqual(self.issues, [])
        self.assertTrue(all(stream.closed for stream in self.streams))

    def test_android_position_counter_does_not_strip_custom_emoji(self):
        text = "подарок 👑"
        body = plugin.tl_string(text) + struct.pack("<IIIiiq", 0x1cb5c415, 1, 0xc8cf05f8, 8, 2, 99999999999999999)
        data = plugin.tl_int(0x751f3146) + body
        message = self.comment(data, text)
        self.assertEqual(message.entities[0].document_id, 99999999999999999)
        self.assertNotEqual(self.streams[-1].getPosition(), len(data))
        self.assertEqual(self.streams[-1].remaining(), 0)

    def test_bare_and_boxed_custom_emoji_keep_large_ids_and_utf16_offsets(self):
        body = plugin.tl_string("A🎉❤️") + struct.pack("<II", 0x1cb5c415, 2)
        body += struct.pack("<Iiiq", 0xc8cf05f8, 1, 2, 6665644666666635010)
        body += struct.pack("<Iiiq", 0xc8cf05f8, 3, 2, 6665644666666635011)
        for data in (body, struct.pack("<I", 0x751f3146) + body):
            with self.subTest(boxed=len(data) > len(body)):
                value = self.comment(data, "A🎉❤️")
                self.assertEqual([(entity.offset, entity.length, entity.document_id) for entity in value.entities],
                                 [(1, 2, 6665644666666635010), (3, 2, 6665644666666635011)])
        self.assertEqual(self.issues, [])

    def test_long_tl_strings_with_emoji_are_not_mistaken_for_constructor(self):
        text = "👑" * 70
        body = plugin.tl_string(text) + struct.pack("<II", 0x1cb5c415, 0)
        self.assertEqual(body[0], 254)
        self.assertEqual(self.comment(body, text).text, text)
        self.assertEqual(self.issues, [])

    def test_bad_comment_encoding_keeps_text_instead_of_hiding_gift(self):
        body = bytes.fromhex("04d0bad18300000015c4b51c00000000")
        for data in (body[:-4], body + b"extra123", b"\x00" * 4,
                     plugin.tl_string("wrong text") + struct.pack("<II", 0x1cb5c415, 0)):
            with self.subTest(data=data):
                value = self.comment(data)
                self.assertEqual(value.text, "ку")
                self.assertEqual(value.entities, [])
        self.assertEqual(len(self.issues), 4)
        self.assertTrue(all(stream.closed for stream in self.streams))

    def test_unsupported_entity_does_not_abort_entire_gift(self):
        body = plugin.tl_string("ку") + struct.pack("<IIIii", 0x1cb5c415, 1, 0x12345678, 0, 1)
        value = self.comment(body)
        self.assertEqual(value.text, "ку")
        self.assertEqual(value.entities, [])
        self.assertEqual(len(self.issues), 1)

    def test_invalid_utf16_bounds_do_not_reach_native_span_renderer(self):
        body = plugin.tl_string("ку") + struct.pack("<IIIiiq", 0x1cb5c415, 1, 0xc8cf05f8, 3, 2, 99999)
        value = self.comment(body)
        self.assertEqual(value.entities, [])
        self.assertEqual(len(self.issues), 1)

if __name__ == "__main__":
    unittest.main()
