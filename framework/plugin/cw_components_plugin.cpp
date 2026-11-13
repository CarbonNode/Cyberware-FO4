// cw_components_plugin.cpp - FO4-crafting-style component counting (BETA QA, the lead, 2026-11-08:
// "Use the workshop/inventory component counts the way FO4 crafting does").
//
// CountComponent: the raw component items in the player's inventory PLUS what the junk there
// breaks down into (every MISC's component list). ConsumeComponents: vanilla scrap-on-craft —
// raw components first, then junk items, returning each scrapped item's OTHER components to the
// inventory, exactly like crafting at a workbench.
//
// GameEnv (main.cpp) forwards its CountComponent / ConsumeComponents overrides here, passing its
// own form resolver so this unit stays free of the formmap.
#include "PCH.h"

#include "../core/cw_core.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace cwcomp {

using Resolver = RE::TESForm* (*)(const std::string& key);

namespace {

    // Every inventory stack: (base object, count). Uses the same public surface main.cpp already
    // relies on (BGSInventoryList under the ref's inventoryList, read-locked).
    template <class F>
    void EachStack(RE::PlayerCharacter* p, F&& fn)
    {
        if (!p || !p->inventoryList) return;
        RE::BSAutoReadLock lock{ p->inventoryList->rwLock };
        for (auto& item : p->inventoryList->data) {
            if (!item.object) continue;
            std::uint32_t n = 0;   // CLF4 a08c731: BGSInventoryItem{ object, stackData } linked stacks
            for (auto* st = item.stackData.get(); st; st = st->nextStack.get()) n += st->count;
            if (n) fn(item.object, n);
        }
    }

    // obj's component yield of `comp` (0 when obj is not a MISC or lists none).
    // componentData: BSTArray<BSTTuple<TESForm*, BGSTypedFormValuePair::SharedVal>>* (a08c731).
    // The entries reference CMPO records (c_Circuitry 01FA9B...), NOT the raw scrap MISC items —
    // verified against Fallout4.esm + the vendored BGSComponent header (BETA QA 2026-11-09).
    int YieldOf(RE::TESBoundObject* obj, RE::TESForm* comp)
    {
        auto* misc = obj ? obj->As<RE::TESObjectMISC>() : nullptr;
        if (!misc || !misc->componentData) return 0;
        for (auto& pair : *misc->componentData)
            if (pair.first == comp) return (int)pair.second.i;
        return 0;
    }

    // The item the player actually carries for `comp`: a CMPO's scrapItem (c_Circuitry ->
    // c_Circuitry_scrap), or the form itself when the cost names a plain MISC (the military chip).
    RE::TESBoundObject* RawItemOf(RE::TESForm* comp)
    {
        if (auto* c = comp ? comp->As<RE::BGSComponent>() : nullptr; c && c->scrapItem) return c->scrapItem;
        return comp ? comp->As<RE::TESBoundObject>() : nullptr;
    }

}  // namespace

int Count(RE::PlayerCharacter* p, RE::TESForm* comp)
{
    if (!p || !comp) return 0;
    int total = 0;
    auto* raw = RawItemOf(comp);
    EachStack(p, [&](RE::TESBoundObject* obj, std::uint32_t n) {
        if (obj == raw) total += (int)n;                           // the raw scrap item itself
        else total += YieldOf(obj, comp) * (int)n;                  // junk that scraps into it
    });
    return total;
}

bool Consume(RE::PlayerCharacter* p, Resolver resolve, const std::vector<std::pair<std::string, int>>& need)
{
    if (!p) return false;
    // Silent (the lead, 2026-11-09): scrapping junk would otherwise flash a feed of removed/added
    // item lines top-left; the upgrade confirm shows the bill instead.
    RE::PlayerCharacter::ScopedInventoryChangeMessageContext silent(true, true);
    // Verify first so a half-taken bill can't happen.
    std::vector<std::pair<RE::TESForm*, int>> bill;
    for (auto& [key, n] : need) {
        RE::TESForm* comp = resolve(key);
        if (!comp || Count(p, comp) < n) return false;
        bill.push_back({ comp, n });
    }
    for (auto& [comp, wanted] : bill) {
        int left = wanted;
        auto* raw = RawItemOf(comp);
        // 1) raw component items
        if (raw) {
            const int have = (int)p->GetInventoryObjectCount(raw);
            const int take = std::min(have, left);
            if (take > 0) {
                RE::TESObjectREFR::RemoveItemData rd(raw, take);
                p->RemoveItem(rd);
                left -= take;
            }
        }
        // 2) scrap junk, smallest yield first (waste as little as vanilla does), returning the
        //    item's OTHER components and any surplus of this one.
        while (left > 0) {
            RE::TESBoundObject* best = nullptr;
            int bestYield = 0;
            EachStack(p, [&](RE::TESBoundObject* obj, std::uint32_t) {
                const int y = YieldOf(obj, comp);
                if (y > 0 && (!best || y < bestYield)) { best = obj; bestYield = y; }
            });
            if (!best) { REX::ERROR("components: ran out while consuming (should have been pre-checked)"); return false; }
            auto* misc = best->As<RE::TESObjectMISC>();
            RE::TESObjectREFR::RemoveItemData rd(best, 1);
            p->RemoveItem(rd);
            // give back everything the junk contains except what this bill still owes — as the
            // RAW scrap items (a CMPO is not an inventory item; its scrapItem is)
            if (misc && misc->componentData)
                for (auto& pair : *misc->componentData) {
                    int give = (int)pair.second.i;
                    if (pair.first == comp) {
                        const int used = std::min(give, left);
                        left -= used;
                        give -= used;
                    }
                    auto* po = give > 0 ? RawItemOf(pair.first) : nullptr;
                    if (po) p->AddObjectToContainer(po, RE::BSTSmartPointer<RE::ExtraDataList>{}, give, nullptr, RE::ITEM_REMOVE_REASON::kNone);
                }
        }
        REX::INFO("components: consumed {} x {}", wanted, comp->GetFormEditorID() ? comp->GetFormEditorID() : "component");
    }
    return true;
}

}  // namespace cwcomp
