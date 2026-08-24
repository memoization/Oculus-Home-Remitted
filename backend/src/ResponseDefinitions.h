#pragma once
// Single source of truth for Oculus Home 2 graphql persisted-query doc_ids. luckily doc_ids are global!!
// A client never sends query text but only a numeric doc_id and variables.
//
// This header is the one place that maps every known doc_id to a named constant the code uses and to a short op name with a one-line purpose for readable logging and reference.

#include <array>
#include <string_view>

namespace home2hook {

// ---- Named doc_id constants ----
// The numeric values live here and nowhere else (ResponseStore's kDoc* alias to these).
namespace doc {

// A. Session / login / config
inline constexpr const char* WorldLogin      = "2474314252665145"; // gates startup
inline constexpr const char* SetUserOptions  = "2659373614156793";
inline constexpr const char* NuxUpdate       = "3425891407451837";
inline constexpr const char* Templates       = "2470834406377364";
inline constexpr const char* Templates2      = "2617284248330218";

// B. World lifecycle
inline constexpr const char* WorldCreate      = "2229535317067212";
inline constexpr const char* UpdateNameWorld  = "2254519954639578";
inline constexpr const char* SetDefaultWorld  = "3121238591227357";
inline constexpr const char* AddWorldVisit    = "2793108640763332";
inline constexpr const char* WorldLockedEdit  = "2507097196064477";
inline constexpr const char* WorldsMarkSeen   = "2451059878250562";
inline constexpr const char* WorldDelete      = "2213511855433466";
inline constexpr const char* WorldLikeToggle  = "2285528538195458";

// C. Object editing (the core edit action)
inline constexpr const char* WorldBatchUpdate = "3393266397380374";

// D. World content & discovery
inline constexpr const char* WorldContent     = "2021902227865170";
inline constexpr const char* WorldsPoll       = "2324561257653109";
inline constexpr const char* WorldKeepAlive   = "2784629574894815"; // per-world heartbeat
inline constexpr const char* WorldsFeedFriends= "2606950666055204";
inline constexpr const char* WorldsFeedPaged  = "2357418124386821";
inline constexpr const char* NuxModules       = "2358278477615362";
inline constexpr const char* Currency         = "2297391550372593";
inline constexpr const char* Announcements    = "2478487052219194";
inline constexpr const char* DefaultWorld     = "3313418545373770";

// E. Item definitions / inventory / apps / UGC endorse
inline constexpr const char* ItemDefs         = "2340400929361818";
inline constexpr const char* Inventory        = "3098640583495832";
inline constexpr const char* WorldsApps       = "3420023344706951";
inline constexpr const char* WorldsGuestApps  = "2426930340689946";
inline constexpr const char* UgcEndorsed      = "2199360326856076";
inline constexpr const char* AddUgcEndorsed   = "2781336295234285";
inline constexpr const char* RemoveUgcEndorsed= "2550601315017673";
inline constexpr const char* SetLastInvView   = "1358143700976653";

// F. Users / social
inline constexpr const char* WorldsList       = "2517010291730152"; // user node incl. worlds.nodes[]
inline constexpr const char* UserNodeLight    = "2687696671294861";
inline constexpr const char* UserFriendReqs   = "3344194228954366";
inline constexpr const char* UsersBatch       = "2970898679591058";
inline constexpr const char* NodeById         = "2305014842947625";
inline constexpr const char* RegisterRanking  = "2638687409503988";

}

// ---- Registry: doc_id to short name and one-line purpose ----
struct DocIdInfo
{
    std::string_view id;
    std::string_view name;// short op name, the response data.<key> where applicable
    std::string_view purpose; // one-line human-readable purpose
};

inline constexpr std::array<DocIdInfo, 38> kDocRegistry = {{
    // A. Session / login / config
    { doc::WorldLogin,       "world_login",            "startup config/login: server_time, limits, default material defs, user_options, nux (failure blocks startup)" },
    { doc::SetUserOptions,   "set_user_options",       "persist client prefs (user_options = base64 JSON)" },
    { doc::NuxUpdate,        "nux_update",             "onboarding/tutorial progress (per-lesson state)" },
    { doc::Templates,        "templates",              "world templates list (served empty)" },
    { doc::Templates2,       "templates_2",            "world templates list variant (served empty)" },
    // B. World lifecycle
    { doc::WorldCreate,      "world_create",           "create a new empty world, returns {world:{id}}" },
    { doc::UpdateNameWorld,  "update_name_world",      "rename a world (name = base64(urlencode))" },
    { doc::SetDefaultWorld,  "set_default_world",      "set the home/default world (in-VR write)" },
    { doc::AddWorldVisit,    "add_world_visit_history","record entering a world (ack)" },
    { doc::WorldLockedEdit,  "world_set_user_locked_edit","lock/unlock world editing" },
    { doc::WorldsMarkSeen,   "worlds_mark_items_seen", "clear new-item badges on owned items" },
    { doc::WorldDelete,      "world_delete",           "delete a world" },
    { doc::WorldLikeToggle,  "world_like_toggle",      "like/unlike a world" },
    // C. Object editing
    { doc::WorldBatchUpdate, "world_batch_update_objects","core edit: create[]/update[]/delete[] objects with world_customizations (b64)" },
    // D. World content & discovery
    { doc::WorldContent,     "world_content",          "authoritative saved world layout: objects.nodes[] with customizations" },
    { doc::WorldsPoll,       "worlds_poll",            "recurring ~5s worlds-by-node-ids refresh" },
    { doc::WorldKeepAlive,   "world_keep_alive",       "per-world heartbeat for the current world" },
    { doc::WorldsFeedFriends,"worlds_feed_friends",    "world discovery feed (friends'/all worlds)" },
    { doc::WorldsFeedPaged,  "worlds_feed_paged",      "paged worlds feed (count and cursor)" },
    { doc::NuxModules,       "nux_modules",            "my_world_data.nux_module_definitions (onboarding lessons)" },
    { doc::Currency,         "currency",               "my_world_data.currency_amount" },
    { doc::Announcements,    "announcements",          "my_world_data.all_announcements" },
    { doc::DefaultWorld,     "default_world",          "read the user's default world id" },
    // E. Item definitions / inventory / apps / UGC endorse
    { doc::ItemDefs,         "item_defs",              "item definitions: def to asset_key/bounds/flags, UGC adds hash_from_client and CDN uris" },
    { doc::Inventory,        "inventory",              "my_world_data.owned_items[] (owned item defs with placed-UGC ownership ids)" },
    { doc::WorldsApps,       "worlds_apps_and_achievements","app and achievement metadata for placed app tiles" },
    { doc::WorldsGuestApps,  "worlds_guest_apps_and_achievements","guest variant of app/achievement metadata" },
    { doc::UgcEndorsed,      "ugc_endorsed",           "my_world_data.local_ugc_items_endorsed (endorsed UGC hashes)" },
    { doc::AddUgcEndorsed,   "add_local_ugc_items_endorsed","endorse UGC hashes (add)" },
    { doc::RemoveUgcEndorsed,"remove_local_ugc_items_endorsed","un-endorse UGC hashes (remove)" },
    { doc::SetLastInvView,   "set_last_inventory_view_time","mark the inventory as viewed" },
    // F. Users / social
    { doc::WorldsList,       "worlds_list",            "user node incl. the user's worlds.nodes[] (the worlds list)" },
    { doc::UserNodeLight,    "user_node_light",        "light user node (display_name/alias/presence)" },
    { doc::UserFriendReqs,   "user_friend_requests",   "user node: friend_requests (received/sent)" },
    { doc::UsersBatch,       "users_batch",            "batch user profiles (user_ids[])" },
    { doc::NodeById,         "node_by_id",             "single node fetch (visited world/entity)" },
    { doc::RegisterRanking,  "register_world_ranking_event","world-tile impression telemetry" },
}};


inline const DocIdInfo* LookupDocId(std::string_view id)
{
    for (const auto& e : kDocRegistry)
        if (e.id == id)
            return &e;
    return nullptr;
}

// Short op name for logs, or "unknown" if the doc_id isn't in the registry.
inline std::string_view DocIdName(std::string_view id)
{
    const DocIdInfo* d = LookupDocId(id);
    return d ? d->name : std::string_view("unknown");
}

// One-line purpose for reference, or "unknown doc_id" if not registered.
inline std::string_view DocIdPurpose(std::string_view id)
{
    const DocIdInfo* d = LookupDocId(id);
    return d ? d->purpose : std::string_view("unknown doc_id");
}

}
