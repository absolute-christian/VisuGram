import types
import unittest
from unittest.mock import patch

from test_profile import plugin

CONTROLLER = "org.telegram.ui.Stars.StarsController"
SHEET = "org.telegram.ui.Gifts.SendGiftSheet"
GIFT = "org.telegram.tgnet.tl.TL_stars$StarGift"
TEXT = "org.telegram.tgnet.TLRPC$TL_textWithEntities"
CALLBACK = "org.telegram.messenger.Utilities$Callback2"
FORM = "org.telegram.tgnet.TLRPC$TL_payments_paymentFormStarGift"

class Method:
    def __init__(self, name, parameters=(), value=None):
        self.name, self.parameters, self.value = name, parameters, value

    def getName(self):
        return self.name

    def getParameterTypes(self):
        return [types.SimpleNamespace(getName=lambda name=name: name) for name in self.parameters]

    def setAccessible(self, value):
        self.accessible = value

    def invoke(self, target):
        return self.value

class Class:
    def __init__(self, methods, parent=None):
        self.methods, self.parent = methods, parent

    def getDeclaredMethods(self):
        return self.methods

    def getSuperclass(self):
        return self.parent

def client_classes(premium=True, buy=True):
    methods = [Method("buyResellingGift", [FORM, GIFT, "long", CALLBACK])]
    if buy:
        methods.append(Method("buyStarGift", [GIFT, "boolean", "boolean", "long", TEXT, CALLBACK]))
    return {CONTROLLER: Class(methods), SHEET: Class(
        [Method("buyPremiumTier")] if premium else [], Class([Method("show")])),
        "org.telegram.ui.Stars.StarsIntroActivity": Class([Method("updateBalance")])}

class GiftHookTests(unittest.TestCase):
    def setUp(self):
        self.subject = plugin.VisuGramPlugin()
        self.subject._loaded = True
        self.subject.settings["visual_gifts"] = False
        self.registered, self.installed = [], []
        self.subject.add_hook = lambda name, **options: self.registered.append((name, options))
        self.subject.hook_method = self.install

    def install(self, method, handler):
        self.installed.append((method, handler))
        return object()

    def test_telegram_1251_without_new_resale_overload_can_buy_gifts(self):
        classes = client_classes()
        with patch.object(plugin, "find_class", side_effect=classes.get):
            self.subject._install_gifts()
        self.subject._gift_toggle(True)
        self.assertTrue(self.subject.settings["visual_gifts"])
        self.assertTrue(self.subject._gift_resale)
        self.assertTrue(all(options["match_substring"] for name, options in self.registered))
        self.assertIn("TL_payments_saveStarGift", [name for name, options in self.registered])

    def test_missing_private_premium_method_uses_inherited_sheet_guard(self):
        classes = client_classes(premium=False)
        with patch.object(plugin, "find_class", side_effect=classes.get):
            self.subject._install_gifts()
        self.assertTrue(self.subject._gift_native)
        method, handler = next(item for item in self.installed if item[0].name == "show")
        self.subject.settings["visual_gifts"] = True
        blocked = []
        clazz = types.SimpleNamespace(getName=lambda: SHEET)
        param = types.SimpleNamespace(thisObject=types.SimpleNamespace(premiumTier=object(), getClass=lambda: clazz),
                                      setResult=lambda value: blocked.append(value))
        with patch.object(self.subject, "_show_error") as error:
            handler.before_hooked_method(param)
            error.assert_called_once_with("PREMIUM_UNAVAILABLE")
        self.assertEqual(blocked, [None])
        param.thisObject.premiumTier = None
        handler.before_hooked_method(param)
        self.assertEqual(blocked, [None])
        param.thisObject.premiumTier = object()
        clazz.getName = lambda: "another.BottomSheet"
        clazz.getSuperclass = lambda: None
        handler.before_hooked_method(param)
        self.assertEqual(blocked, [None])

    def test_cosmetic_hook_failure_does_not_remove_purchase_hooks(self):
        classes = client_classes()

        def install(method, handler):
            if isinstance(handler, plugin.GiftBalanceHook):
                raise RuntimeError("inlined display method")
            return self.install(method, handler)

        self.subject.hook_method = install
        with patch.object(plugin, "find_class", side_effect=classes.get):
            self.subject._install_gifts()
        self.assertTrue(self.subject._gift_native)
        self.assertTrue(self.subject._payment_guard)
        self.assertIn("inlined display method", self.subject._gift_report())

    def test_sheet_fallback_preserves_recipient_and_rich_message(self):
        classes = client_classes(buy=False)
        message = types.SimpleNamespace(text="emoji", entities=object())
        classes[SHEET].methods.extend([Method("buyStarGift"), Method("getMessage", value=message)])
        with patch.object(plugin, "find_class", side_effect=classes.get):
            self.subject._install_gifts()
        self.assertTrue(self.subject._gift_native)
        handler = next(handler for method, handler in self.installed if isinstance(handler, plugin.SheetGiftBuyHook))
        self.subject.settings["visual_gifts"] = True
        stopped, payload = [], object()
        gift = types.SimpleNamespace(stars=25)
        sheet = types.SimpleNamespace(currentAccount=1, starGift=gift, upgrade=False, dialogId=202, anonymous=True)
        param = types.SimpleNamespace(thisObject=sheet, setResult=lambda value: stopped.append(value))
        with patch.object(plugin, "gift_payload", return_value=payload) as encoder, \
             patch.object(self.subject, "_send_visual") as send:
            handler.before_hooked_method(param)
        self.assertEqual(stopped, [None])
        encoder.assert_called_once_with(gift, 202, message, True, 25, "XTR")
        self.assertEqual(send.call_args.args[:2], (1, payload))

    def test_sheet_encoding_failure_still_stops_the_real_purchase(self):
        self.subject.settings["visual_gifts"] = True
        stopped = []
        sheet = types.SimpleNamespace(currentAccount=0, starGift=types.SimpleNamespace(stars=25),
                                      upgrade=False, dialogId=202, anonymous=False)
        param = types.SimpleNamespace(thisObject=sheet, setResult=lambda value: stopped.append(value))
        handler = plugin.SheetGiftBuyHook(self.subject, Method("getMessage"))
        with patch.object(plugin, "gift_payload", side_effect=ValueError("invalid")), \
             patch.object(self.subject, "_send_visual") as send, patch.object(self.subject, "_show_error"), \
             patch.object(self.subject, "_gift_callback") as callback:
            handler.before_hooked_method(param)
        self.assertEqual(stopped, [None])
        send.assert_not_called()
        self.assertFalse(callback.call_args.args[1])

    def test_failed_payment_registration_cannot_enable_visual_mode(self):
        self.subject.add_hook = lambda *args, **kwargs: (_ for _ in ()).throw(RuntimeError("SDK hook rejected"))
        self.subject._install_gifts()
        with patch.object(self.subject, "_show_gift_report") as report:
            self.subject._gift_toggle(True)
        self.assertFalse(self.subject.settings["visual_gifts"])
        self.assertFalse(self.subject._payment_guard)
        self.assertIn("SDK hook rejected", self.subject._gift_report())
        report.assert_called_once()

    def test_missing_core_hooks_keep_mode_off_and_report_the_missing_method(self):
        with patch.object(plugin, "find_class", return_value=None):
            self.subject._install_gifts()
        with patch.object(self.subject, "_show_gift_report"):
            self.subject._gift_toggle(True)
        self.assertFalse(self.subject.settings["visual_gifts"])
        self.assertIn("StarsController.buyStarGift: method not found", self.subject._gift_report())

    def test_legacy_class_wrapper_uses_the_application_class_loader(self):
        clazz = Class([Method("buyPremiumTier")])
        calls = []
        loader = object()
        java = types.ModuleType("java.lang")
        java.Class = types.SimpleNamespace(forName=lambda *args: calls.append(args) or clazz)
        messenger = types.ModuleType("org.telegram.messenger")
        messenger.ApplicationLoader = types.SimpleNamespace(applicationContext=types.SimpleNamespace(getClassLoader=lambda: loader))
        with patch.object(plugin, "find_class", return_value=object()), \
             patch.dict("sys.modules", {"java.lang": java, "org.telegram.messenger": messenger}):
            method = plugin.find_method(SHEET, "buyPremiumTier", [])
        self.assertIs(method, clazz.methods[0])
        self.assertEqual(calls, [(SHEET, False, loader)])

    def test_layered_payment_requests_and_real_gift_mutations_are_blocked(self):
        self.subject.settings["visual_gifts"] = True
        with patch.object(plugin, "run_on_ui_thread"):
            for name in ("TL_payments_getPaymentForm", "TL_payments_canPurchaseStore", "TL_payments_saveStarGift",
                         "TL_payments_sendStarsForm_layer222", "TL_payments_transferStarGift"):
                self.assertEqual(self.subject.pre_request_hook(name, 0, object()).strategy, "cancel")
            self.assertFalse(hasattr(self.subject.pre_request_hook("TL_payments_getStarGifts", 0, object()), "strategy"))
            self.subject.settings["visual_gifts"] = False
            self.assertFalse(hasattr(self.subject.pre_request_hook("TL_payments_sendStarsForm", 0, object()), "strategy"))

    def test_shop_reports_gift_toggle_separately_from_synchronization(self):
        self.subject._gift_native = True
        with patch.object(self.subject, "_show_error") as error:
            self.subject._open_shop()
        error.assert_called_once_with("VISUAL_DISABLED")

    def test_loading_with_missing_purchase_hooks_turns_off_the_saved_visual_flag(self):
        self.subject.settings["visual_gifts"] = True

        def missing():
            self.subject._payment_guard, self.subject._gift_native = True, False

        with patch.object(self.subject, "_install_native"), patch.object(self.subject, "_install_gifts", side_effect=missing), \
             patch.object(self.subject, "_restart"):
            self.subject.on_plugin_load()
        self.assertFalse(self.subject.settings["visual_gifts"])

    def test_unknown_signature_is_reported_without_guessing_argument_positions(self):
        classes = client_classes(buy=False)
        classes[CONTROLLER].methods.append(Method("buyStarGift", [GIFT, "long", CALLBACK]))
        with patch.object(plugin, "find_class", side_effect=classes.get):
            self.subject._install_gifts()
        self.assertFalse(self.subject._gift_native)
        self.assertIn("unsupported signature: (TL_stars$StarGift, long, Utilities$Callback2)", self.subject._gift_report())

if __name__ == "__main__":
    unittest.main()
