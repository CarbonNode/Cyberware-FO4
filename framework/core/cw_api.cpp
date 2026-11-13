// cw_api.cpp - request/response dispatch for the PrismaUI view (docs/UI-API.md).
#include "cw_core.h"

namespace cw {

json Api::Event(const std::string& type) const
{
    Derived d = engine_->Compute(engine_->state());
    json e = { { "type", type } };
    e["state"] = engine_->StateView(engine_->state(), d);
    if (type == "open") e["shop"] = engine_->ShopView(engine_->state(), d);
    return e;
}

json Api::HandleJson(const json& req)
{
    json res;
    res["id"] = req.is_object() && req.contains("id") ? req["id"] : json(nullptr);
    try {
        if (!req.is_object()) throw CwError("E_BAD_REQUEST", "request must be a JSON object");
        std::string op = req.value("op", "");
        json args = req.value("args", json::object());
        if (!args.is_object()) throw CwError("E_BAD_REQUEST", "'args' must be an object");
        bool dryRun = req.value("dryRun", false);
        res["op"] = op;
        if (op == "hello") {
            res["data"] = { { "apiVersion", kApiVersion }, { "pluginVersion", pluginVersion }, { "buildId", buildId },
                            { "ops", { "hello", "getCatalog", "getState", "getShop", "buy", "buyGear", "buyMod", "sell", "sellMod", "install", "remove",
                                       "upgrade", "attachMod", "detachMod", "setVariant", "buyCyberPerk", "refundCyberPerk", "reroll", "rerollResolve",
                                       "useCapacityItem", "buyCapacityItem", "setSettings", "setCustomize", "resetCustomize", "setLoadout", "close" } } };
        } else if (op == "getCatalog") {
            res["data"] = engine_->CatalogView();
        } else if (op == "getState") {
            Derived d = engine_->Compute(engine_->state());
            res["state"] = engine_->StateView(engine_->state(), d);
        } else if (op == "getShop") {
            Derived d = engine_->Compute(engine_->state());
            res["data"] = engine_->ShopView(engine_->state(), d);
            res["state"] = engine_->StateView(engine_->state(), d);
        } else if (op == "close") {
            engine_->currentRipperdoc.clear();
            if (onClose) onClose();
            res["data"] = json::object();
        } else {
            json r = engine_->Run(op, args, dryRun);
            res["data"] = r["data"];
            res["state"] = r["state"];
            if (dryRun) res["dryRun"] = true;
            else {
                Derived d = engine_->Compute(engine_->state());
                res["shop"] = engine_->ShopView(engine_->state(), d);
            }
        }
        res["ok"] = true;
    } catch (const CwError& e) {
        res["ok"] = false;
        res["error"] = { { "code", e.code }, { "message", e.what() } };
    } catch (const std::exception& e) {
        res["ok"] = false;
        res["error"] = { { "code", "E_INTERNAL" }, { "message", e.what() } };
    }
    return res;
}

std::string Api::Handle(const std::string& request)
{
    json req;
    try {
        req = json::parse(request);
    } catch (const std::exception& e) {
        json res = { { "id", nullptr }, { "ok", false }, { "error", { { "code", "E_BAD_REQUEST" }, { "message", std::string("not JSON: ") + e.what() } } } };
        return res.dump();
    }
    // dump with error_handler replace: never throw on odd bytes coming back from the game (names are ASCII anyway)
    return HandleJson(req).dump(-1, ' ', false, json::error_handler_t::replace);
}

}  // namespace cw
