// cw_pkgdiag.cpp - {"cmd":"packages","ref":"<hex>"} (test seam, bDebug=1 + sCmdFile only): an actor's AI packages in order,
// read straight from the forms on the game thread, so ONE call answers "order, schedule or target?" (CLINIC P14 and P15
// each spent a boot per hypothesis). Default actor: Varga (CW_RipperdocRef_diamondcity).
//
// What it reads: the base NPC's package list (TESAIForm::aiPackList, in order), each package's PSDT schedule against the
// game clock, each condition item (TESConditionItem::IsTrue, the engine's own condition code), the package's location and
// target fields, and the process's running package + its resolved target. What it does NOT do: no Papyrus VM call (the
// leash: DispatchMethodCall stays in cw_voice.cpp only, docs/LESSONS.md), no package pick (CommonLibF4 @a08c731 has no
// Actor::EvaluatePackage), nothing written. Alias, scene and quest-override packages sit outside the base list; the summary
// says so when the running package is not in it.
//
// Log lines (stable; INTEGRATION and CLINIC grep them):
//   cmd: packages actor <ID> '<name>' base <ID> hour <h.hh> date <yyyy-mm-dd> dow <0-6> running <ID> <EditorID|?> count <n>
//   cmd: package #<i> <ID> <EditorID|?> conditions=pass|fail|? schedule=any|in|out|? running=yes|no window=<text>
//        psdt=<month>/<dow>/<date>/<hour>/<min>/<duration> type=<n> fail=<#k,..|-> unknown=<#k,..|-> loc=set|none targ=set|none data=set|none
//   cmd: package #<i> cond #<k> <Function>(<p1>,<p2>) <op> <value> run=<subject> or=<0|1> -> yes|no|? [why]
//   cmd: packages first-eligible #<i> <ID> <EditorID|?> running #<i>|not-in-list target <ID|none>
// <ID> is the runtime form id, 8 uppercase hex, followed by its EditorID (CLINIC's form: "FE47B96E CW_ClinicSleep0to6").
// "first-eligible" = the first package in order whose conditions pass and whose schedule covers now: the engine's pick
// also weighs package state and the overrides above, so it is a guide, not the engine's answer.
#include "cw_pkgdiag.h"
#include "cw_pkgsched.h"

#include <cstdio>
#include <format>
#include <string>
#include <vector>

namespace pkgdiag
{
    namespace
    {
        std::string g_clinicPlugin = "Cyberware - Clinic.esp";
        std::function<std::string(std::uint32_t)> g_lookup;

        // Varga's packages (Cyberware - Clinic.esp local ids, clinic.lock.json; CLINIC, 2026-11-09). FO4 drops
        // EditorIDs at runtime, so the seam names them from this table, in CLINIC's form "<ID> <EditorID>".
        struct Known { std::uint32_t local; const char* edid; };
        constexpr Known kClinic[] = {
            { 0x96E, "CW_ClinicSleep0to6" },  { 0x973, "CW_ClinicOffice6to10" }, { 0x974, "CW_ClinicWalk10to14" },
            { 0x975, "CW_ClinicShop14to24" }, { 0xA09, "CW_ClinicOutside" },     { 0xA4C, "CW_ClinicIndoorHold" },
            { 0xA51, "CW_ClinicStepIn" },     // INSTALL SCENE step-in (CLINIC, 2026-11-10); keep in step with cw_clinic.cpp PackageName
        };

        std::string Hex(std::uint32_t id) { char b[12]; std::snprintf(b, sizeof(b), "%08X", id); return b; }

        std::string EditorId(const RE::TESForm* f)
        {
            if (!f) return "?";
            if (const char* e = f->GetFormEditorID(); e && *e) return e;
            const std::uint32_t id = f->GetFormID();
            if (auto* dh = RE::TESDataHandler::GetSingleton())
                for (const auto& k : kClinic)
                    if (auto* g = dh->LookupForm(k.local, g_clinicPlugin); g && g->GetFormID() == id) return k.edid;
            if (g_lookup) { std::string s = g_lookup(id); if (!s.empty()) return s; }
            return "?";
        }
        std::string Named(const RE::TESForm* f) { return f ? Hex(f->GetFormID()) + " " + EditorId(f) : std::string("00000000 none"); }

        const char* OpName(int c)
        {
            static const char* k[] = { "==", "!=", ">", ">=", "<", "<=" };
            return (c >= 0 && c < 6) ? k[c] : "op?";
        }
        const char* RunName(RE::CONDITIONITEMOBJECT o)
        {
            switch (o) {
            case RE::CONDITIONITEMOBJECT::kSelf: return "subject";
            case RE::CONDITIONITEMOBJECT::kTarget: return "target";
            case RE::CONDITIONITEMOBJECT::kRef: return "ref";
            case RE::CONDITIONITEMOBJECT::kCombatTarget: return "combattarget";
            case RE::CONDITIONITEMOBJECT::kLinkedRef: return "linkedref";
            case RE::CONDITIONITEMOBJECT::kQuestAlias: return "alias";
            case RE::CONDITIONITEMOBJECT::kPackData: return "packdata";
            case RE::CONDITIONITEMOBJECT::kEventData: return "eventdata";
            default: return "other";
            }
        }

        // The condition function's name and parameters. A function index past the table, or a parameter type that is not a
        // form, prints as a number: only a 64-bit pointer of a form-typed parameter is read as a form.
        std::string FuncText(const RE::TESConditionItem& it)
        {
            const auto fn = (std::uint32_t)it.data.functionData.function.underlying();
            auto table = RE::SCRIPT_FUNCTION::GetScriptFunctions();
            const RE::SCRIPT_FUNCTION* sf = fn < table.size() ? &table[fn] : nullptr;
            std::string s = (sf && sf->functionName && *sf->functionName) ? std::string(sf->functionName) : ("Function" + std::to_string(fn));
            s += '(';
            for (int p = 0; p < 2; ++p) {
                if (!sf || p >= sf->paramCount || !sf->parameters) break;
                if (p) s += ',';
                const auto type = (int)sf->parameters[p].paramType.underlying();
                const auto raw = (std::uintptr_t)it.data.functionData.param[p];
                // SCRIPT_PARAM_TYPE values that hold a number or a string, never a form
                const bool notForm = type == 0x0 || type == 0x1 || type == 0x2 || type == 0x8 || type == 0x12 || type == 0x17 || type == 0x1C ||
                                     type == 0x20 || type == 0x29 || type == 0x2C || type == 0x2D || type == 0x2E || type == 0x2F || type == 0x30 ||
                                     type == 0x3F || type == 0x44;
                if (!notForm && raw > 0xFFFFFFFFull) s += Hex(reinterpret_cast<const RE::TESForm*>(raw)->GetFormID());
                else s += std::to_string((std::int64_t)(std::int32_t)(std::uint32_t)raw);
            }
            return s + ')';
        }
    }

    void SetClinicPlugin(const std::string& clinicCellRef)
    {
        const auto bar = clinicCellRef.find('|');
        if (bar != std::string::npos && bar > 0) g_clinicPlugin = clinicCellRef.substr(0, bar);
    }
    void SetNameLookup(std::function<std::string(std::uint32_t)> fn) { g_lookup = std::move(fn); }

    void Log(RE::Actor* actor)
    {
        if (!actor) { REX::WARN("cmd: packages: no such actor (pass \"ref\":\"<hex form id>\" of a placed actor)"); return; }
        auto* npc = actor->GetNPC();
        auto* cal = RE::Calendar::GetSingleton();
        cw::pkg::Clock clk;
        int year = 0;
        if (cal && cal->gameHour) clk.hour = cal->gameHour->value;
        if (cal && cal->gameMonth) clk.month = (int)cal->gameMonth->value;
        if (cal && cal->gameDay) clk.date = (int)cal->gameDay->value;
        if (cal && cal->gameYear) year = (int)cal->gameYear->value;
        clk.dayOfWeek = cw::pkg::DayOfWeek(year, clk.month, clk.date);

        RE::TESPackage* running = actor->currentProcess ? actor->currentProcess->currentPackage.package : nullptr;
        RE::TESObjectREFR* runTarget = nullptr;
        RE::NiPointer<RE::TESObjectREFR> runTargetHold;
        if (actor->currentProcess) { runTargetHold = actor->currentProcess->currentPackage.target.get(); runTarget = runTargetHold.get(); }

        std::vector<RE::TESPackage*> list;
        if (npc) for (auto* p : npc->aiPackList.listPackages) list.push_back(p);
        const char* nm = actor->GetDisplayFullName();
        REX::INFO("cmd: packages actor {} '{}' base {} hour {:.2f} date {:04d}-{:02d}-{:02d} dow {} running {} count {}", Hex(actor->GetFormID()), nm ? nm : "",
                  Hex(npc ? npc->GetFormID() : 0), clk.hour, year, clk.month + 1, clk.date, clk.dayOfWeek, Named(running), list.size());

        int firstEligible = -1, runningIdx = -1;
        for (std::size_t i = 0; i < list.size(); ++i) {
            RE::TESPackage* pk = list[i];
            if (!pk) { REX::INFO("cmd: package #{} 00000000 none (empty slot)", i); continue; }
            const auto& ps = pk->packSched.psData;
            const cw::pkg::Sched s{ ps.month, ps.dayOfWeek, ps.date, ps.hour, ps.min, ps.duration };
            const cw::pkg::Tri sm = cw::pkg::Matches(s, clk);

            std::vector<cw::pkg::Item> items;
            std::vector<std::string> condLines;
            int k = 0;
            for (RE::TESConditionItem* it = pk->packConditions.head; it; it = it->next, ++k) {
                const auto obj = static_cast<RE::CONDITIONITEMOBJECT>(it->data.object.underlying());
                cw::pkg::Item ci;
                ci.orNext = it->data.compareOr != 0;
                std::string why;
                // Items that need a quest, an event or package data in scope are not evaluated outside a real pick.
                const bool scoped = it->data.aliasParams || it->data.packDataParams || obj == RE::CONDITIONITEMOBJECT::kQuestAlias ||
                                    obj == RE::CONDITIONITEMOBJECT::kPackData || obj == RE::CONDITIONITEMOBJECT::kEventData ||
                                    obj == RE::CONDITIONITEMOBJECT::kCommandTarget || obj == RE::CONDITIONITEMOBJECT::kEventCameraRef ||
                                    obj == RE::CONDITIONITEMOBJECT::kMyKiller || obj == RE::CONDITIONITEMOBJECT::kCombatTarget;
                const bool noGlobal = it->data.valueIsGlobal && !it->data.global;
                if (scoped) why = " [not evaluated: needs a quest/alias/package-data/event scope]";
                else if (noGlobal) why = " [not evaluated: its global is missing]";
                else {
                    // the subject is the actor; a target-run item has no package target outside a pick, so it runs on her too
                    ci.result = it->IsTrue(actor, actor) ? cw::pkg::Tri::Yes : cw::pkg::Tri::No;
                    if (obj == RE::CONDITIONITEMOBJECT::kTarget) why = " [target taken as the actor]";
                }
                const float val = it->data.valueIsGlobal ? (it->data.global ? it->data.global->value : 0.0f) : it->data.value;
                char vb[32]; std::snprintf(vb, sizeof(vb), "%.2f", val);
                const std::string glob = it->data.valueIsGlobal ? " (global " + Hex(it->data.global ? it->data.global->GetFormID() : 0) + ")" : std::string();
                condLines.push_back(std::format("cmd: package #{} cond #{} {} {} {}{} run={} or={} -> {}{}", i, k, FuncText(*it), OpName(it->data.condition), vb,
                                                glob, RunName(obj), (int)ci.orNext, cw::pkg::TriName(ci.result), why));
                items.push_back(ci);
            }
            const auto cmb = cw::pkg::Combine(items);
            const bool isRunning = pk == running;
            if (isRunning && runningIdx < 0) runningIdx = (int)i;
            if (firstEligible < 0 && cmb.pass == cw::pkg::Tri::Yes && sm == cw::pkg::Tri::Yes) firstEligible = (int)i;
            REX::INFO("cmd: package #{} {} conditions={} schedule={} running={} window={} psdt={}/{}/{}/{}/{}/{} type={} fail={} unknown={} loc={} targ={} data={}",
                      i, Named(pk), cw::pkg::CondWord(cmb.pass), cw::pkg::SchedWord(s, sm), isRunning ? "yes" : "no", cw::pkg::WindowText(s),
                      s.month, s.dayOfWeek, s.date, s.hour, s.min, s.duration, (int)pk->data.packType, cw::pkg::IndexList(cmb.failing),
                      cw::pkg::IndexList(cmb.unknown), pk->packLoc ? "set" : "none", pk->packTarg ? "set" : "none", pk->packData ? "set" : "none");
            for (const auto& l : condLines) REX::INFO("{}", l);
        }
        const std::string fe = firstEligible >= 0 ? ("#" + std::to_string(firstEligible) + " " + Named(list[firstEligible])) : std::string("none");
        const std::string ri = runningIdx >= 0 ? ("#" + std::to_string(runningIdx)) : std::string(running ? "not-in-list (alias, scene or override package)" : "none");
        REX::INFO("cmd: packages first-eligible {} running {} target {}", fe, ri, runTarget ? Hex(runTarget->GetFormID()) : std::string("none"));
    }
}
