import base64
import hashlib
import html
import json
import os
import re
import secrets
import sqlite3
import threading
import time
import uuid
from collections import defaultdict, deque
from contextlib import contextmanager
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime
from decimal import Decimal, InvalidOperation
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.error import HTTPError, URLError
from urllib.parse import urlparse
from urllib.request import Request, urlopen


class ApiError(Exception):
    def __init__(self, status, code):
        self.status = status
        self.code = code


def canonical(kind, value):
    if not isinstance(value, str):
        raise ApiError(400, "INVALID_ASSET")
    if kind == "username":
        value = value.strip().lstrip("@").lower()
        if not re.fullmatch(r"[a-z][a-z0-9_]{3,31}", value):
            raise ApiError(400, "INVALID_USERNAME")
    elif kind == "phone":
        value = re.sub(r"[\s()+-]", "", value)
        if not re.fullmatch(r"888[0-9]{8}", value):
            raise ApiError(400, "INVALID_PHONE")
        value = "+" + value
    elif kind == "nft":
        if not re.fullmatch(r"[A-Za-z0-9]+-[1-9][0-9]{0,11}", value):
            raise ApiError(400, "INVALID_NFT")
        value = value.lower()
    else:
        raise ApiError(400, "INVALID_ASSET")
    return value


def text(value, limit):
    if not isinstance(value, str) or len(value) > limit:
        raise ApiError(400, "INVALID_TEXT")
    return value


def account(value):
    value = str(value)
    if not re.fullmatch(r"[1-9][0-9]{0,15}", value):
        raise ApiError(400, "INVALID_ACCOUNT")
    return value


def page(url):
    request = Request(url, headers={"User-Agent": "Mozilla/5.0 VisuGram/1.0"})
    try:
        with urlopen(request, timeout=10) as response:
            if urlparse(response.url).hostname != urlparse(url).hostname:
                raise ApiError(503, "SOURCE_UNAVAILABLE")
            content = response.read(1_500_001)
            if len(content) > 1_500_000:
                raise ApiError(503, "SOURCE_UNAVAILABLE")
            return content.decode("utf-8", "replace")
    except HTTPError as error:
        if error.code == 404:
            return None
        raise ApiError(503, "SOURCE_UNAVAILABLE") from error
    except (URLError, TimeoutError, OSError) as error:
        raise ApiError(503, "SOURCE_UNAVAILABLE") from error


def plain(value):
    return html.unescape(re.sub(r"<[^>]+>", "", value)).strip()


def fragment_metadata(kind, value):
    url = "https://fragment.com/" + ("number/" + value[1:] if kind == "phone" else "username/" + value)
    content = page(url)
    now = int(time.time())
    result = {"entity": value, "url": url, "checked_at": now, "currency": "TON", "fiat_currency": "", "fiat_amount": 0}
    if content is None:
        if kind == "phone":
            raise ApiError(422, "PHONE_NOT_FOUND")
        result.update(status="unlisted", price_source="generated", price_kind="visual", crypto_amount=str(secrets.randbelow(4) * 1_000_000_000 + 7_000_000_000), date=now)
        return result
    section = re.search(r'<section\b[^>]*class="[^"]*tm-auction-section[^"]*"[^>]*>(.*?)</section>', content, re.S)
    if not section:
        raise ApiError(503, "SOURCE_UNAVAILABLE")
    section = section.group(1)
    domain = re.search(r'class="tm-section-header-domain"[^>]*>(.*?)</span>\s*(?:</span>)*', section, re.S)
    if kind == "phone" and (not domain or re.sub(r"\D", "", plain(domain.group(1))) != value[1:]):
        raise ApiError(503, "SOURCE_UNAVAILABLE")
    status = re.search(r'class="[^"]*tm-section-header-status[^"]*"[^>]*>(.*?)</span>', section, re.S)
    status = plain(status.group(1)).lower() if status else ""
    statuses = {"sold": ("sold", "last_sale"), "on auction": ("auction", "bid"), "for sale": ("sale", "asking"), "available": ("available", "minimum_bid")}
    if status not in statuses:
        if kind == "phone":
            raise ApiError(422, "PHONE_NOT_FOUND") if status in ("unavailable", "not available") else ApiError(503, "SOURCE_UNAVAILABLE")
        if status in ("unavailable", "not available"):
            result.update(status="unlisted", price_source="generated", price_kind="visual", crypto_amount=str(secrets.randbelow(4) * 1_000_000_000 + 7_000_000_000), date=now)
            return result
        raise ApiError(503, "SOURCE_UNAVAILABLE")
    price = re.search(r'class="[^"]*\bicon-ton\b[^"]*"[^>]*>(.*?)</div>', section, re.S)
    if not price:
        raise ApiError(503, "SOURCE_UNAVAILABLE")
    try:
        amount = Decimal(plain(price.group(1)).replace(",", "").replace(" ", ""))
        if not amount.is_finite() or amount < 0 or amount > 1_000_000_000:
            raise InvalidOperation()
    except InvalidOperation as error:
        raise ApiError(503, "SOURCE_UNAVAILABLE") from error
    date = re.search(r'Purchased on\s*<time\b[^>]*datetime="([^"]+)"', section)
    result.update(status=statuses[status][0], price_kind=statuses[status][1], price_source="fragment", crypto_amount=str(int(amount * 1_000_000_000)), date=int(datetime.fromisoformat(date.group(1)).timestamp()) if date else now)
    return result


class Store:
    def __init__(self, filename):
        self.filename = filename
        self.metadata_locks = [threading.Lock() for _ in range(32)]
        self.metadata_pool = ThreadPoolExecutor(max_workers=8)
        Path(filename).parent.mkdir(parents=True, exist_ok=True)
        with self.connect() as db:
            db.executescript("""
                PRAGMA journal_mode=WAL;
                CREATE TABLE IF NOT EXISTS users(id TEXT PRIMARY KEY, username TEXT NOT NULL DEFAULT '');
                CREATE TABLE IF NOT EXISTS profiles(user_id TEXT PRIMARY KEY, data TEXT NOT NULL);
                CREATE TABLE IF NOT EXISTS assets(kind TEXT NOT NULL, value TEXT NOT NULL, owner_id TEXT NOT NULL, PRIMARY KEY(kind,value));
                CREATE TABLE IF NOT EXISTS metadata(kind TEXT NOT NULL, value TEXT NOT NULL, data TEXT NOT NULL, PRIMARY KEY(kind,value));
                CREATE TABLE IF NOT EXISTS gifts(id TEXT PRIMARY KEY, sender_id TEXT NOT NULL, recipient_id TEXT NOT NULL, slug TEXT NOT NULL, data TEXT NOT NULL);
                CREATE INDEX IF NOT EXISTS gifts_sender ON gifts(sender_id);
                CREATE INDEX IF NOT EXISTS gifts_recipient ON gifts(recipient_id);
                CREATE TABLE IF NOT EXISTS operations(user_id TEXT NOT NULL, operation_id TEXT NOT NULL, request_hash TEXT NOT NULL, result TEXT NOT NULL, PRIMARY KEY(user_id,operation_id));
                CREATE TABLE IF NOT EXISTS events(seq INTEGER PRIMARY KEY AUTOINCREMENT, user_id TEXT NOT NULL);
                CREATE INDEX IF NOT EXISTS events_user ON events(user_id,seq);
            """)

    @contextmanager
    def connect(self):
        db = sqlite3.connect(self.filename, timeout=10, isolation_level=None)
        db.row_factory = sqlite3.Row
        try:
            yield db
        finally:
            db.close()

    def metadata(self, kind, value):
        lock = self.metadata_locks[int(hashlib.sha256((kind + value).encode()).hexdigest(), 16) % 32]
        with lock:
            with self.connect() as db:
                row = db.execute("SELECT data FROM metadata WHERE kind=? AND value=?", (kind, value)).fetchone()
                previous = json.loads(row[0]) if row else None
                if previous and previous["checked_at"] + (86400 if previous["price_source"] == "generated" else 300) > time.time():
                    return previous
            result = fragment_metadata(kind, value)
            if previous and previous["price_source"] == result["price_source"] == "generated":
                result.update(crypto_amount=previous["crypto_amount"], date=previous["date"])
            with self.connect() as db:
                db.execute("INSERT INTO metadata VALUES(?,?,?) ON CONFLICT(kind,value) DO UPDATE SET data=excluded.data", (kind, value, json.dumps(result)))
            return result

    def event(self, db, user):
        db.execute("INSERT INTO events(user_id) VALUES(?)", (user,))

    def profile(self, db, user):
        row = db.execute("SELECT data FROM profiles WHERE user_id=?", (user,)).fetchone()
        return json.loads(row[0]) if row else {"phone": "", "usernames": [], "primary": "", "collectibles": {}, "revision": 0}

    def claim(self, db, kind, value, owner):
        row = db.execute("SELECT owner_id FROM assets WHERE kind=? AND value=?", (kind, value)).fetchone()
        if row and row[0] != owner:
            raise ApiError(409, "ASSET_TAKEN")
        db.execute("INSERT OR IGNORE INTO assets VALUES(?,?,?)", (kind, value, owner))

    def mutation(self, user, operation, body, action):
        if not re.fullmatch(r"[A-Za-z0-9-]{16,64}", operation):
            raise ApiError(400, "INVALID_OPERATION")
        digest = hashlib.sha256(json.dumps(body, sort_keys=True).encode()).hexdigest()
        with self.connect() as db:
            db.execute("BEGIN IMMEDIATE")
            try:
                row = db.execute("SELECT request_hash,result FROM operations WHERE user_id=? AND operation_id=?", (user, operation)).fetchone()
                if row:
                    if row[0] != digest:
                        raise ApiError(409, "OPERATION_REUSED")
                    db.rollback()
                    return json.loads(row[1])
                result = action(db)
                db.execute("INSERT INTO operations VALUES(?,?,?,?)", (user, operation, digest, json.dumps(result)))
                db.commit()
                return result
            except Exception:
                db.rollback()
                raise

    def update_profile(self, user, body):
        phone = canonical("phone", body["phone"]) if body.get("phone") else ""
        names = body.get("usernames", [])
        if not isinstance(names, list) or len(names) > 20:
            raise ApiError(400, "INVALID_USERNAME")
        names = [canonical("username", name) for name in names]
        if len(set(names)) != len(names):
            raise ApiError(400, "INVALID_USERNAME")
        primary = canonical("username", body["primary"]) if body.get("primary") else ""
        if primary and primary not in names:
            raise ApiError(400, "INVALID_PRIMARY")
        lookups = [("username", name) for name in names] + ([("phone", phone)] if phone else [])
        futures = [(kind + ":" + value, self.metadata_pool.submit(self.metadata, kind, value)) for kind, value in lookups]
        deadline = time.monotonic() + 35
        try:
            metadata = {key: future.result(timeout=max(0, deadline - time.monotonic())) for key, future in futures}
        except TimeoutError as error:
            for _, future in futures:
                future.cancel()
            raise ApiError(503, "SOURCE_UNAVAILABLE") from error

        def update(db):
            previous = self.profile(db, user)
            if int(body.get("revision", -1)) != previous["revision"]:
                raise ApiError(409, "PROFILE_CHANGED")
            for name in names:
                self.claim(db, "username", name, user)
            if phone:
                self.claim(db, "phone", phone, user)
            for name in previous["usernames"]:
                if name not in names:
                    db.execute("DELETE FROM assets WHERE kind='username' AND value=? AND owner_id=?", (name, user))
            if previous["phone"] and previous["phone"] != phone:
                db.execute("DELETE FROM assets WHERE kind='phone' AND value=? AND owner_id=?", (previous["phone"], user))
            profile = {"phone": phone, "usernames": names, "primary": primary, "collectibles": metadata, "revision": previous["revision"] + 1}
            db.execute("INSERT INTO profiles VALUES(?,?) ON CONFLICT(user_id) DO UPDATE SET data=excluded.data", (user, json.dumps(profile)))
            self.event(db, user)
            return {"profile": profile}

        return self.mutation(user, body.get("operation_id", ""), body, update)

    def send_gift(self, user, body):
        recipient = account(body.get("recipient_id", ""))
        source = text(body.get("source", ""), 16384)
        try:
            decoded = base64.b64decode(source, validate=True)
            if not decoded or len(decoded) % 4:
                raise ValueError()
        except (ValueError, TypeError) as error:
            raise ApiError(400, "INVALID_GIFT") from error
        slug = canonical("nft", body["slug"]) if body.get("slug") else ""
        caption = text(body.get("message", ""), 255)
        price = str(body.get("price", "0"))
        if not re.fullmatch(r"[0-9]{1,18}", price):
            raise ApiError(400, "INVALID_PRICE")
        currency = body.get("currency", "XTR")
        if currency not in ("XTR", "TON"):
            raise ApiError(400, "INVALID_PRICE")

        def send(db):
            if not db.execute("SELECT 1 FROM users WHERE id=?", (recipient,)).fetchone():
                raise ApiError(409, "RECIPIENT_NOT_CONNECTED")
            if any(db.execute("SELECT COUNT(*) FROM gifts WHERE sender_id=? OR recipient_id=?", (owner, owner)).fetchone()[0] >= 1000 for owner in {user, recipient}):
                raise ApiError(409, "GIFT_LIMIT")
            if slug:
                owner = db.execute("SELECT owner_id FROM assets WHERE kind='nft' AND value=?", (slug,)).fetchone()
                if owner:
                    if owner[0] != user or recipient == user:
                        raise ApiError(409, "ASSET_TAKEN")
                    for row in db.execute("SELECT id,data FROM gifts WHERE slug=?", (slug,)).fetchall():
                        old = json.loads(row["data"])
                        old.update(active=False, pinned=False)
                        db.execute("UPDATE gifts SET data=? WHERE id=?", (json.dumps(old), row["id"]))
                        self.event(db, old["sender_id"])
                        self.event(db, old["recipient_id"])
                    db.execute("UPDATE assets SET owner_id=? WHERE kind='nft' AND value=?", (recipient, slug))
                else:
                    self.claim(db, "nft", slug, recipient)
            gift = {"id": str(uuid.uuid4()), "sender_id": user, "recipient_id": recipient, "source": source, "slug": slug, "message": caption, "anonymous": bool(body.get("anonymous")), "price": price, "currency": currency, "date": int(time.time()), "pinned": False, "hidden": False, "active": True}
            db.execute("INSERT INTO gifts VALUES(?,?,?,?,?)", (gift["id"], user, recipient, slug, json.dumps(gift)))
            self.event(db, user)
            if recipient != user:
                self.event(db, recipient)
            return {"gift": gift}

        return self.mutation(user, body.get("operation_id", ""), body, send)

    def manage_gift(self, user, body):
        gift_id = text(body.get("id", ""), 64)

        def manage(db):
            row = db.execute("SELECT data FROM gifts WHERE id=? AND recipient_id=?", (gift_id, user)).fetchone()
            if not row:
                raise ApiError(403, "NOT_GIFT_OWNER")
            gift = json.loads(row[0])
            if not gift["active"]:
                raise ApiError(409, "NOT_GIFT_OWNER")
            for flag in ("pinned", "hidden"):
                if flag in body:
                    if not isinstance(body[flag], bool):
                        raise ApiError(400, "INVALID_GIFT")
                    gift[flag] = body[flag]
            if gift["hidden"]:
                gift["pinned"] = False
            if gift["pinned"]:
                if not gift["slug"]:
                    raise ApiError(400, "INVALID_GIFT")
                gift["hidden"] = False
                others = [json.loads(r[0]) for r in db.execute("SELECT data FROM gifts WHERE recipient_id=? AND id<>?", (user, gift_id))]
                if sum(g["pinned"] and g["active"] for g in others) >= 6:
                    raise ApiError(409, "PIN_LIMIT")
            db.execute("UPDATE gifts SET data=? WHERE id=?", (json.dumps(gift), gift_id))
            self.event(db, user)
            if gift["sender_id"] != user:
                self.event(db, gift["sender_id"])
            return {"gift": gift}

        return self.mutation(user, body.get("operation_id", ""), body, manage)

    def sync(self, user, body):
        username = text(body.get("telegram_username", ""), 32)
        with self.connect() as db:
            db.execute("BEGIN")
            try:
                db.execute("INSERT INTO users VALUES(?,?) ON CONFLICT(id) DO UPDATE SET username=excluded.username", (user, username))
                cursor = db.execute("SELECT COALESCE(MAX(seq),0) FROM events WHERE user_id=?", (user,)).fetchone()[0]
                if str(body.get("cursor", "")) == str(cursor):
                    result = {"cursor": str(cursor), "unchanged": True}
                else:
                    gifts = [json.loads(row[0]) for row in db.execute("SELECT data FROM gifts WHERE sender_id=? OR recipient_id=? ORDER BY rowid", (user, user))]
                    result = {"cursor": str(cursor), "profile": self.profile(db, user), "gifts": gifts}
                db.commit()
                return result
            except Exception:
                db.rollback()
                raise

    def view(self, owner, user):
        with self.connect() as db:
            db.execute("BEGIN")
            profile = self.profile(db, owner)
            gifts = [json.loads(row[0]) for row in db.execute("SELECT data FROM gifts WHERE recipient_id=? ORDER BY rowid", (owner,))]
            for gift in gifts:
                if gift["anonymous"] and gift["sender_id"] != user:
                    gift["sender_id"] = "0"
            db.commit()
            return {"profile": profile, "gifts": [gift for gift in gifts if gift["active"] and not gift["hidden"]]}


class Server(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, address, store):
        super().__init__(address, Handler)
        self.store = store
        self.rate_lock = threading.Lock()
        self.rate = defaultdict(deque)
        self.slots = threading.BoundedSemaphore(32)

    def process_request(self, request, address):
        if not self.slots.acquire(False):
            request.close()
            return
        try:
            super().process_request(request, address)
        except Exception:
            self.slots.release()
            raise

    def process_request_thread(self, request, address):
        try:
            super().process_request_thread(request, address)
        finally:
            self.slots.release()


class Handler(BaseHTTPRequestHandler):
    def setup(self):
        super().setup()
        self.connection.settimeout(15)

    def log_message(self, *_):
        pass

    def respond(self, status, body):
        encoded = json.dumps(body, ensure_ascii=False).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(encoded)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(encoded)

    def do_GET(self):
        self.respond(200 if self.path == "/health" else 404, {"ok": self.path == "/health"})

    def do_POST(self):
        try:
            user = account(self.headers.get("X-VisuGram-User-Id", ""))
            now = time.monotonic()
            with self.server.rate_lock:
                if len(self.server.rate) > 10000:
                    self.server.rate = defaultdict(deque, {key: value for key, value in self.server.rate.items() if value and value[-1] > now - 60})
                for key in ("ip:" + self.client_address[0], "user:" + user):
                    queue = self.server.rate[key]
                    while queue and queue[0] < now - 60:
                        queue.popleft()
                    if len(queue) >= 120:
                        raise ApiError(429, "RATE_LIMIT")
                    queue.append(now)
            length = int(self.headers.get("Content-Length", "0"))
            if length <= 0 or length > 262144:
                raise ApiError(413, "REQUEST_TOO_LARGE")
            body = json.loads(self.rfile.read(length))
            if not isinstance(body, dict):
                raise ApiError(400, "INVALID_REQUEST")
            routes = {"/v1/sync": self.server.store.sync, "/v1/profile": self.server.store.update_profile, "/v1/gifts": self.server.store.send_gift, "/v1/gifts/manage": self.server.store.manage_gift}
            if self.path == "/v1/profile/view":
                result = self.server.store.view(account(body.get("owner_id", "")), user)
            elif self.path in routes:
                result = routes[self.path](user, body)
            else:
                raise ApiError(404, "NOT_FOUND")
            self.respond(200, result)
        except ApiError as error:
            self.respond(error.status, {"error": error.code})
        except (ValueError, KeyError, TypeError, UnicodeError):
            self.respond(400, {"error": "INVALID_REQUEST"})
        except sqlite3.Error:
            self.respond(503, {"error": "DATABASE_UNAVAILABLE"})
        except Exception:
            self.respond(500, {"error": "SERVER_ERROR"})


if __name__ == "__main__":
    filename = os.environ.get("VISUGRAM_DATABASE", "data/visugram.sqlite3")
    Server((os.environ.get("VISUGRAM_BIND", "127.0.0.1"), int(os.environ.get("PORT", os.environ.get("VISUGRAM_PORT", "8080")))), Store(filename)).serve_forever()
