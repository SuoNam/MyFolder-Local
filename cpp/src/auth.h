// Included after the shared DB/HTTP helpers in main.cpp. Credentials never enter JSON responses.
static constexpr int REGISTRATION_TTL = 7 * 86400;
static constexpr int DEVICE_TTL = 90 * 86400;
static constexpr int DEVICE_IDLE_TTL = 30 * 86400;

static void initialize_auth(Db& db) {
    db.raw(R"SQL(
CREATE TABLE IF NOT EXISTS trusted_devices (
 id INTEGER PRIMARY KEY,user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
 name TEXT NOT NULL,token_hash TEXT NOT NULL UNIQUE,previous_hash TEXT,previous_until INTEGER NOT NULL DEFAULT 0,
 status TEXT NOT NULL CHECK(status IN ('pending','approved','rejected','revoked')),
 created_at INTEGER NOT NULL,expires_at INTEGER NOT NULL,last_used_at INTEGER NOT NULL,
 reviewed_at INTEGER,reviewer_id INTEGER REFERENCES users(id) ON DELETE SET NULL);
CREATE INDEX IF NOT EXISTS trusted_devices_user ON trusted_devices(user_id);
CREATE TABLE IF NOT EXISTS registration_requests (
 id INTEGER PRIMARY KEY,username TEXT NOT NULL,password_hash TEXT NOT NULL,receipt_hash TEXT NOT NULL UNIQUE,
 device_name TEXT NOT NULL,trust_device INTEGER NOT NULL DEFAULT 0,
 status TEXT NOT NULL CHECK(status IN ('pending','approved','rejected')),
 created_at INTEGER NOT NULL,expires_at INTEGER NOT NULL,reviewed_at INTEGER,
 reviewer_id INTEGER REFERENCES users(id) ON DELETE SET NULL,user_id INTEGER REFERENCES users(id) ON DELETE SET NULL,
 claimed_at INTEGER);
CREATE UNIQUE INDEX IF NOT EXISTS registration_pending_name ON registration_requests(username) WHERE status='pending';
)SQL");
    bool has_device = false;
    for (const auto& column : db.query("PRAGMA table_info(sessions)")) has_device |= column["name"] == "device_id";
    if (!has_device) db.raw("ALTER TABLE sessions ADD COLUMN device_id INTEGER REFERENCES trusted_devices(id) ON DELETE CASCADE");
}

static bool secure_request(const httplib::Request& req) {
    // Only explicitly trusted reverse proxies may assert HTTPS. Never trust a public client's header.
    if (req.get_header_value("X-Forwarded-Proto") != "https") return false;
    std::istringstream proxies(env("MYFOLDER_TRUSTED_PROXY_IPS", "127.0.0.1,::1"));
    std::string address;
    while (std::getline(proxies, address, ',')) if (trim(address) == req.remote_addr) return true;
    return false;
}
static bool device_login_available(const httplib::Request& req) {
    // Local is HTTP-ready by default; deployments may explicitly require HTTPS for persistent credentials.
    return secure_request(req) || env("MYFOLDER_REQUIRE_HTTPS_DEVICE_LOGIN", "0") != "1";
}
static void require_device_transport(const httplib::Request& req) {
    if (!device_login_available(req)) fail(400, "管理员已开启仅 HTTPS 免密登录，请通过可信 HTTPS 入口访问");
}
static void auth_cookie(const httplib::Request& req, httplib::Response& res, const std::string& name,
                        const std::string& value, int age = -1) {
    res.set_header("Set-Cookie", name + "=" + value + "; Path=/; HttpOnly; SameSite=Strict" +
        (secure_request(req) ? "; Secure" : "") + (age >= 0 ? "; Max-Age=" + std::to_string(age) : ""));
}
static void auth_rate_limit(const httplib::Request& req, const std::string& action, int maximum, int seconds) {
    static std::mutex mutex;
    static std::map<std::string, std::pair<int64_t, int>> attempts;
    std::lock_guard<std::mutex> lock(mutex);
    const auto time = now();
    auto address = req.remote_addr;
    if (secure_request(req)) {
        // A trusted edge proxy must append the actual peer, not accept an arbitrary client-supplied chain.
        auto forwarded = req.get_header_value("X-Forwarded-For");
        auto comma = forwarded.rfind(',');
        auto last = trim(comma == std::string::npos ? forwarded : forwarded.substr(comma + 1));
        if (!last.empty() && last.size() <= 64 && std::all_of(last.begin(), last.end(), [](unsigned char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F') || c == ':' || c == '.';
        })) address = last;
    }
    const auto key = action + ":" + address;
    if (attempts.size() > 4096) {
        for (auto it = attempts.begin(); it != attempts.end();) {
            if (time - it->second.first > 3600) it = attempts.erase(it); else ++it;
        }
        if (attempts.size() > 4096) fail(429, "请求过多，请稍后重试");
    }
    auto& entry = attempts[key];
    if (!entry.first || time - entry.first >= seconds) entry = {time, 0};
    if (++entry.second > maximum) fail(429, "尝试次数过多，请稍后重试");
}
static std::string auth_device_name(const json& body) {
    auto name = trim(as_string(body, "device_name"));
    if (name.empty() || name.size() > 120 || std::any_of(name.begin(), name.end(), [](unsigned char c) { return c < 32; }))
        fail(400, "请填写不超过 120 字节的设备名称");
    return name;
}
static void issue_session(Db& db, const json& user, const httplib::Request& req, httplib::Response& res,
                          const json& device_id = nullptr) {
    const auto token = random_hex(32);
    db.query("DELETE FROM sessions WHERE expires_at<=?", json::array({now()}));
    db.query("INSERT INTO sessions(token_hash,user_id,expires_at,device_id) VALUES(?,?,?,?)",
             json::array({sha256(token), user["id"], now() + 86400, device_id}));
    auth_cookie(req, res, "session", token);
}
static void revoke_user_devices(Db& db, const json& id) {
    db.query("UPDATE trusted_devices SET status='revoked' WHERE user_id=? AND status IN ('pending','approved')", json::array({id}));
    db.query("DELETE FROM sessions WHERE user_id=?", json::array({id}));
    db.query("UPDATE registration_requests SET claimed_at=? WHERE user_id=? AND claimed_at IS NULL", json::array({now(), id}));
}
static json device_public(const json& row) {
    auto status = as_string(row, "status");
    if ((status == "pending" || status == "approved") &&
        (as_int(row, "expires_at") <= now() || (status == "approved" && as_int(row, "last_used_at") + DEVICE_IDLE_TTL <= now()))) status = "expired";
    json out;
    for (const auto& key : {"id", "user_id", "name", "created_at", "expires_at", "last_used_at", "reviewed_at"}) out[key] = row[key];
    out["status"] = status;
    if (row.contains("username")) out["username"] = row["username"];
    if (row.contains("role")) out["role"] = role_of(row);
    return out;
}
static bool may_review_device(const json& actor, const json& target) {
    return role_of(actor) == "superadmin" || (role_of(actor) == "admin" && as_string(target, "role") == "user");
}

// Caller holds BEGIN IMMEDIATE and has verified this receipt's ownership, approval and expiry.
static json claim_registration(Db& db, const json& row, const json& user,
                               const httplib::Request& req, httplib::Response& res) {
    json device_id = nullptr;
    if (row.value("trust_device", 0)) {
        require_device_transport(req);
        auto token = random_hex(32);
        db.query("INSERT INTO trusted_devices(user_id,name,token_hash,status,created_at,expires_at,last_used_at,reviewed_at,reviewer_id) VALUES(?,?,?,'approved',?,?,?,?,?)",
                 json::array({user["id"], row["device_name"], sha256(token), now(), now() + DEVICE_TTL, now(), row["reviewed_at"], row["reviewer_id"]}));
        device_id = db.id();
        auth_cookie(req, res, "mf_device", token, DEVICE_TTL);
    }
    db.query("UPDATE registration_requests SET claimed_at=? WHERE id=?", json::array({now(), row["id"]}));
    return device_id;
}

static json password_login(Db& db, const json& user, const httplib::Request& req, httplib::Response& res) {
    json device_id = nullptr;
    std::string state = "missing";
    bool claimed = false;
    db.raw("BEGIN IMMEDIATE");
    try {
        const auto current = db.one("SELECT password,reset_until FROM users WHERE id=?", json::array({user["id"]}));
        if (current.is_null() || current["password"] != user["password"] || current["reset_until"] != user["reset_until"])
            fail(401, "账号状态已变化，请重新登录");
        const auto receipt = request_cookie(req, "mf_registration");
        const auto registration = db.one("SELECT * FROM registration_requests WHERE receipt_hash=?",
                                         json::array({sha256(receipt)}));
        if (!receipt.empty() && !registration.is_null() && registration["user_id"] == user["id"] &&
            registration["status"] == "approved" && registration["claimed_at"].is_null() &&
            as_int(registration, "expires_at") > now() && !user.value("reset_until", 0)) {
            if (!registration.value("trust_device", 0) || device_login_available(req)) {
                device_id = claim_registration(db, registration, user, req, res);
                claimed = true;
            } else state = "https_required"; // Password login stays available; keep the approved receipt unclaimed.
        }
        if (device_id.is_null() && !user.value("reset_until", 0)) {
            const auto token = request_cookie(req, "mf_device");
            const auto device = db.one("SELECT * FROM trusted_devices WHERE user_id=? AND token_hash=?",
                                       json::array({user["id"], sha256(token)}));
            if (!token.empty() && !device.is_null()) {
                state = as_string(device_public(device), "status");
                if (state == "approved") {
                    if (device_login_available(req)) {
                        device_id = device["id"];
                        db.query("UPDATE trusted_devices SET last_used_at=? WHERE id=?", json::array({now(), device_id}));
                    } else state = "https_required";
                }
            }
        }
        issue_session(db, user, req, res, device_id);
        db.raw("COMMIT");
    } catch (...) { db.raw("ROLLBACK"); throw; }
    if (claimed) auth_cookie(req, res, "mf_registration", "", 0);
    auto result = public_user(user);
    result["browser_login"] = device_id.is_null() ? state : "approved";
    return result;
}

static bool public_auth(Db& db, const httplib::Request& req, httplib::Response& res) {
    if (req.path == "/api/logout" && req.method == "POST") {
        // Even an expired session must be able to forget this browser's persistent credential.
        db.raw("BEGIN IMMEDIATE");
        try {
            db.query("DELETE FROM sessions WHERE token_hash=?", json::array({sha256(session_cookie(req))}));
            auto token = request_cookie(req, "mf_device");
            if (!token.empty()) {
                auto device = db.one("SELECT id FROM trusted_devices WHERE token_hash=? OR (previous_hash=? AND previous_until>?)",
                                     json::array({sha256(token), sha256(token), now()}));
                if (!device.is_null()) {
                    db.query("UPDATE trusted_devices SET status='revoked' WHERE id=?", json::array({device["id"]}));
                    db.query("DELETE FROM sessions WHERE device_id=?", json::array({device["id"]}));
                }
            }
            db.raw("COMMIT");
        } catch (...) { db.raw("ROLLBACK"); throw; }
        auth_cookie(req, res, "session", "", 0);
        auth_cookie(req, res, "mf_device", "", 0);
        auth_cookie(req, res, "mf_registration", "", 0);
        response_json(res, 200, {{"ok", true}}); return true;
    }
    if (req.path == "/api/auth/options" && req.method == "GET") {
        response_json(res, 200, {{"registration", true}, {"trusted_login", device_login_available(req)},
                                 {"secure_transport", secure_request(req)}, {"device_days", 90}});
        return true;
    }
    if (req.path == "/api/register" && req.method == "POST") {
        auth_rate_limit(req, "register", 5, 3600);
        auto body = json_body(req);
        auto username = trim(as_string(body, "username")), password = as_string(body, "password");
        if (username.size() < 3 || username.size() > 64 || !std::all_of(username.begin(), username.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        })) fail(400, "账号需为 3–64 位字母、数字、下划线或减号");
        if (password.size() < 8 || password.size() > 1024) fail(400, "密码需为 8–1024 字节");
        const bool trust = body.value("trust_device", false);
        if (trust) require_device_transport(req);
        const auto name = auth_device_name(body);
        const auto hashed = password_hash(password), receipt = random_hex(32);
        db.raw("BEGIN IMMEDIATE");
        try {
            db.query("UPDATE registration_requests SET status='rejected',password_hash='' WHERE status='pending' AND expires_at<=?", json::array({now()}));
            if (!db.one("SELECT id FROM users WHERE username=?", json::array({username})).is_null() ||
                !db.one("SELECT id FROM registration_requests WHERE username=? AND status='pending'", json::array({username})).is_null())
                fail(409, "该账号已被使用或已有待审核申请");
            if (as_int(db.one("SELECT COUNT(*) AS n FROM registration_requests WHERE status='pending'"), "n") >= 500)
                fail(429, "待审核申请较多，请联系管理员");
            db.query("INSERT INTO registration_requests(username,password_hash,receipt_hash,device_name,trust_device,status,created_at,expires_at) VALUES(?,?,?,?,?,'pending',?,?)",
                     json::array({username, hashed, sha256(receipt), name, trust, now(), now() + REGISTRATION_TTL}));
            db.raw("COMMIT");
        } catch (...) { db.raw("ROLLBACK"); throw; }
        auth_cookie(req, res, "mf_registration", receipt, REGISTRATION_TTL);
        response_json(res, 201, {{"status", "pending"}, {"username", username}, {"message", "申请已提交，等待管理员审核"}});
        return true;
    }
    if (req.path == "/api/registration/status" && req.method == "GET") {
        auto receipt = request_cookie(req, "mf_registration");
        auto row = db.one("SELECT username,status,expires_at,claimed_at FROM registration_requests WHERE receipt_hash=?", json::array({sha256(receipt)}));
        if (receipt.empty() || row.is_null()) fail(404, "此浏览器没有注册申请");
        auto status = as_int(row, "expires_at") <= now() ? "expired" : as_string(row, "status");
        response_json(res, 200, {{"username", row["username"]}, {"status", status}, {"claimed", !row["claimed_at"].is_null()}});
        return true;
    }
    if (req.path == "/api/registration/activate" && req.method == "POST") {
        auth_rate_limit(req, "activate", 30, 900);
        const auto receipt = request_cookie(req, "mf_registration");
        db.raw("BEGIN IMMEDIATE");
        try {
            auto row = db.one("SELECT * FROM registration_requests WHERE receipt_hash=?", json::array({sha256(receipt)}));
            if (receipt.empty() || row.is_null() || as_int(row, "expires_at") <= now()) fail(401, "注册申请已失效，请使用账号密码登录");
            if (row["status"] != "approved") fail(403, "注册申请尚未通过");
            if (!row["claimed_at"].is_null()) fail(409, "已领取登录资格，请使用账号密码登录");
            auto user = db.one("SELECT * FROM users WHERE id=?", json::array({row["user_id"]}));
            if (user.is_null() || user.value("reset_until", 0)) fail(401, "账号状态已变化，请使用密码登录");
            auto device_id = claim_registration(db, row, user, req, res);
            issue_session(db, user, req, res, device_id);
            db.raw("COMMIT");
            auth_cookie(req, res, "mf_registration", "", 0);
            auto result = public_user(user);
            result["browser_login"] = device_id.is_null() ? "missing" : "approved";
            response_json(res, 200, result);
        } catch (...) { db.raw("ROLLBACK"); throw; }
        return true;
    }
    if (req.path == "/api/auth/device-login" && req.method == "POST") {
        require_device_transport(req);
        auto token = request_cookie(req, "mf_device");
        if (token.empty()) fail(401, "此浏览器没有免密凭据");
        auth_rate_limit(req, "device-login", 30, 900);
        db.raw("BEGIN IMMEDIATE");
        try {
            auto hash = sha256(token);
            auto device = db.one("SELECT * FROM trusted_devices WHERE token_hash=? OR (previous_hash=? AND previous_until>?)", json::array({hash, hash, now()}));
            if (device.is_null() || device_public(device)["status"] != "approved") fail(401, "设备免密资格未获批、已撤销或已过期");
            if (device["token_hash"] != hash) fail(409, "设备凭据刚刚刷新，请重试");
            auto user = db.one("SELECT * FROM users WHERE id=?", json::array({device["user_id"]}));
            if (user.is_null() || user.value("reset_until", 0)) fail(401, "请先使用密码登录并完成密码重置");
            const auto rotated = random_hex(32);
            db.query("UPDATE trusted_devices SET previous_hash=token_hash,previous_until=?,token_hash=?,last_used_at=? WHERE id=?",
                     json::array({now() + 30, sha256(rotated), now(), device["id"]}));
            issue_session(db, user, req, res, device["id"]);
            db.raw("COMMIT");
            auth_cookie(req, res, "mf_device", rotated, static_cast<int>(as_int(device, "expires_at") - now()));
            response_json(res, 200, public_user(user));
        } catch (...) { db.raw("ROLLBACK"); throw; }
        return true;
    }
    return false;
}

static bool authenticated_auth(Db& db, const json& user, const httplib::Request& req, httplib::Response& res) {
    if (req.path == "/api/trusted-devices" && req.method == "GET") {
        json rows = json::array();
        for (const auto& row : db.query("SELECT * FROM trusted_devices WHERE user_id=? ORDER BY created_at DESC LIMIT 200", json::array({user["id"]}))) {
            auto item = device_public(row);
            auto hash = sha256(request_cookie(req, "mf_device"));
            item["current"] = row["token_hash"] == hash || row["previous_hash"] == hash;
            rows.push_back(item);
        }
        response_json(res, 200, {{"devices", rows}, {"trusted_login", device_login_available(req)},
                                 {"secure_transport", secure_request(req)}});
        return true;
    }
    if (req.path == "/api/trusted-devices/request" && req.method == "POST") {
        require_device_transport(req);
        if (user.value("reset_until", 0)) fail(403, "请先完成密码重置");
        auth_rate_limit(req, "device-request", 10, 3600);
        auto name = auth_device_name(json_body(req));
        auto existing = db.one("SELECT * FROM trusted_devices WHERE token_hash=? AND user_id=?", json::array({sha256(request_cookie(req, "mf_device")), user["id"]}));
        if (!existing.is_null() && (device_public(existing)["status"] == "pending" || device_public(existing)["status"] == "approved")) {
            response_json(res, 200, device_public(existing)); return true;
        }
        auto count = db.one("SELECT COUNT(*) AS n FROM trusted_devices WHERE user_id=? AND status IN ('pending','approved') AND expires_at>?", json::array({user["id"], now()}));
        if (as_int(count, "n") >= 20) fail(409, "已有 20 台待审核或可信设备，请先撤销旧设备");
        auto token = random_hex(32);
        db.query("INSERT INTO trusted_devices(user_id,name,token_hash,status,created_at,expires_at,last_used_at) VALUES(?,?,?,'pending',?,?,?)",
                 json::array({user["id"], name, sha256(token), now(), now() + REGISTRATION_TTL, now()}));
        auto device = db.one("SELECT * FROM trusted_devices WHERE id=?", json::array({db.id()}));
        auth_cookie(req, res, "mf_device", token, DEVICE_TTL);
        response_json(res, 201, device_public(device));
        return true;
    }
    if (req.path == "/api/trusted-devices/revoke" && req.method == "POST") {
        auto id = as_int(json_body(req), "id");
        auto target = db.one("SELECT d.*,u.role,u.is_superadmin FROM trusted_devices d JOIN users u ON u.id=d.user_id WHERE d.id=?", json::array({id}));
        if (target.is_null()) fail(404, "设备不存在");
        if (target["user_id"] != user["id"] && !may_review_device(user, target)) fail(403, "没有撤销此设备的权限");
        db.raw("BEGIN IMMEDIATE");
        try {
            db.query("UPDATE trusted_devices SET status='revoked',previous_hash=NULL,previous_until=0 WHERE id=?", json::array({id}));
            db.query("DELETE FROM sessions WHERE device_id=?", json::array({id}));
            db.raw("COMMIT");
        } catch (...) { db.raw("ROLLBACK"); throw; }
        response_json(res, 200, {{"ok", true}}); return true;
    }
    if (req.path.rfind("/api/admin/", 0) != 0) return false;
    require_admin(user);
    if (req.path == "/api/admin/registrations" && req.method == "GET") {
        json rows = json::array();
        for (auto row : db.query("SELECT id,username,device_name,trust_device,status,created_at,expires_at,reviewed_at FROM registration_requests ORDER BY CASE WHEN status='pending' THEN 0 ELSE 1 END,created_at DESC LIMIT 500")) {
            if (row["status"] == "pending" && as_int(row, "expires_at") <= now()) row["status"] = "expired";
            rows.push_back(row);
        }
        response_json(res, 200, {{"requests", rows}}); return true;
    }
    if (req.path == "/api/admin/registrations/review" && req.method == "POST") {
        auto body = json_body(req);
        auto decision = as_string(body, "decision");
        if (decision != "approve" && decision != "reject") fail(400, "审核结果无效");
        db.raw("BEGIN IMMEDIATE");
        try {
            auto row = db.one("SELECT * FROM registration_requests WHERE id=?", json::array({as_int(body, "id")}));
            if (row.is_null()) fail(404, "申请不存在");
            if (row["status"] != "pending" || as_int(row, "expires_at") <= now()) fail(409, "申请已处理或已过期");
            json user_id = nullptr;
            if (decision == "approve") {
                db.query("INSERT INTO users(username,password,role,created_at) VALUES(?,?,'user',?)", json::array({row["username"], row["password_hash"], now()}));
                user_id = db.id(); // No automatic root-directory grant for self-registered users.
            }
            db.query("UPDATE registration_requests SET status=?,password_hash='',reviewed_at=?,reviewer_id=?,user_id=? WHERE id=?",
                     json::array({decision == "approve" ? "approved" : "rejected", now(), user["id"], user_id, row["id"]}));
            db.raw("COMMIT");
        } catch (...) { db.raw("ROLLBACK"); throw; }
        response_json(res, 200, {{"ok", true}}); return true;
    }
    if (req.path == "/api/admin/trusted-devices" && req.method == "GET") {
        json rows = json::array();
        for (const auto& row : db.query("SELECT d.*,u.username,u.role,u.is_superadmin FROM trusted_devices d JOIN users u ON u.id=d.user_id ORDER BY CASE WHEN d.status='pending' THEN 0 ELSE 1 END,d.created_at DESC LIMIT 500"))
            if (may_review_device(user, row)) rows.push_back(device_public(row));
        response_json(res, 200, {{"devices", rows}}); return true;
    }
    if (req.path == "/api/admin/trusted-devices/review" && req.method == "POST") {
        auto body = json_body(req);
        auto decision = as_string(body, "decision");
        if (decision != "approve" && decision != "reject") fail(400, "审核结果无效");
        db.raw("BEGIN IMMEDIATE");
        try {
            auto device = db.one("SELECT d.*,u.role,u.is_superadmin FROM trusted_devices d JOIN users u ON u.id=d.user_id WHERE d.id=?", json::array({as_int(body, "id")}));
            if (device.is_null()) fail(404, "设备不存在");
            if (!may_review_device(user, device)) fail(403, "只有超级管理员可以审核管理员设备");
            if (device_public(device)["status"] != "pending") fail(409, "设备申请已处理或已过期");
            db.query("UPDATE trusted_devices SET status=?,reviewed_at=?,reviewer_id=?,expires_at=?,last_used_at=? WHERE id=?",
                     json::array({decision == "approve" ? "approved" : "rejected", now(), user["id"], now() + DEVICE_TTL, now(), device["id"]}));
            db.raw("COMMIT");
        } catch (...) { db.raw("ROLLBACK"); throw; }
        response_json(res, 200, {{"ok", true}}); return true;
    }
    return false;
}
