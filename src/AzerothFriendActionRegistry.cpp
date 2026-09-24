#include "AzerothFriendActionRegistry.h"
#include <algorithm>
#include <cctype>
#include <set>
#include <sstream>
#include <unordered_map>

namespace AzerothFriendActionRegistry
{
    namespace
    {
        std::string JsonTypeFor(std::string const& key)
        {
            static std::set<std::string> const integers = {
                "id", "quest_id", "questid", "guid", "spellid", "spell_id", "count", "slot",
                "seconds", "radius", "size", "distance", "gold", "silver", "copper"
            };
            static std::set<std::string> const numbers = { "x", "y", "z", "range" };
            static std::set<std::string> const booleans = { "all", "all_junk", "master_target", "override" };
            static std::set<std::string> const arrays = { "add", "remove" };

            if (integers.count(key))
                return "{\"type\":\"integer\",\"minimum\":0}";
            if (numbers.count(key))
                return "{\"type\":\"number\"}";
            if (booleans.count(key))
                return "{\"type\":\"boolean\"}";
            if (arrays.count(key))
                return "{\"type\":\"array\",\"items\":{\"type\":\"string\"}}";
            return "{\"type\":\"string\"}";
        }

        std::string ParamsSchema(std::string params)
        {
            std::vector<std::string> properties;
            std::vector<std::string> required;
            std::vector<std::string> alternativeRequirements;
            std::stringstream ss(params);
            std::string token;
            while (std::getline(ss, token, ','))
            {
                token.erase(std::remove_if(token.begin(), token.end(),
                    [](unsigned char c) { return std::isspace(c) != 0; }), token.end());
                if (token.empty())
                    continue;

                bool optional = token.front() == '[' && token.back() == ']';
                if (optional)
                    token = token.substr(1, token.size() - 2);

                bool const hasAlternatives = token.find('|') != std::string::npos;
                std::stringstream choices(token);
                std::string key;
                std::vector<std::string> choiceKeys;
                while (std::getline(choices, key, '|'))
                {
                    if (key.empty())
                        continue;
                    properties.push_back("\"" + key + "\":" + JsonTypeFor(key));
                    choiceKeys.push_back(key);
                    // A compact `guid|name` parameter means either key is valid;
                    // requiring the first key would reject the second valid form.
                    if (!optional && !hasAlternatives)
                        required.push_back("\"" + key + "\"");
                }
                if (!optional && choiceKeys.size() > 1)
                {
                    std::ostringstream group;
                    group << "{\"anyOf\":[";
                    for (size_t i = 0; i < choiceKeys.size(); ++i)
                    {
                        if (i)
                            group << ',';
                        group << "{\"required\":[\"" << choiceKeys[i] << "\"]}";
                    }
                    group << "]}";
                    alternativeRequirements.push_back(group.str());
                }
            }

            std::ostringstream out;
            out << "{\"type\":\"object\",\"properties\":{";
            for (size_t i = 0; i < properties.size(); ++i)
            {
                if (i)
                    out << ',';
                out << properties[i];
            }
            out << "}";
            if (!required.empty())
            {
                out << ",\"required\":[";
                for (size_t i = 0; i < required.size(); ++i)
                {
                    if (i)
                        out << ',';
                    out << required[i];
                }
                out << ']';
            }
            if (!alternativeRequirements.empty())
            {
                out << ",\"allOf\":[";
                for (size_t i = 0; i < alternativeRequirements.size(); ++i)
                {
                    if (i)
                        out << ',';
                    out << alternativeRequirements[i];
                }
                out << ']';
            }
            out << ",\"additionalProperties\":false}";
            return out.str();
        }

        ActionAuthority AuthorityFor(std::string const& name)
        {
            static std::set<std::string> const ownerOnly = {
                "buy", "sell", "bank", "guild_bank", "mail", "send_mail", "craft",
                "destroy_item", "give_gold", "outfit", "drop_quest", "trainer_learn", "talents",
                "invite", "leave_group", "give_leader", "lfg", "duel_start", "duel_accept",
                "pvp", "trade", "trade_start", "trade_accept", "trade_set_item",
                "trade_clear_item", "trade_set_gold", "playerbot_action", "playerbot_command", "strategy"
            };
            return ownerOnly.count(name) ? ActionAuthority::OwnerCommand : ActionAuthority::Autonomous;
        }

        std::string CompletionFor(std::string const& name)
        {
            static std::set<std::string> const handoffs = {
                "follow", "grind", "wander", "travel_to", "taxi", "rpg_do_quest", "runaway",
                "set_action_mode"
            };
            static std::set<std::string> const observed = {
                "move_to", "go_to", "talk_to", "interact", "use_object", "attack", "wait",
                "accept_quest", "accept_all_quests", "turn_in_quest"
            };
            if (handoffs.count(name))
                return "playerbot_handoff";
            if (observed.count(name))
                return "world_state_postcondition";
            return "dispatch_acknowledged";
        }

        std::string BindingFor(ActionInfo const& info)
        {
            if (info.passthrough)
                return "native_passthrough";
            if (!info.playerbotAction.empty())
                return "playerbot_action";
            if (info.category == "movement")
                return "playerbot_pathfinding";
            static std::set<std::string> const opcodeAdapters = {
                "trade", "trade_start", "trade_accept", "trade_cancel", "trade_set_item",
                "trade_clear_item", "trade_set_gold", "duel_accept", "duel_decline", "dismount"
            };
            if (opcodeAdapters.count(info.name))
                return "client_opcode_adapter";
            return "semantic_adapter";
        }

        std::string PreconditionsFor(ActionInfo const& info)
        {
            std::string value = "bot_in_world,current_control_revision";
            if (info.category == "combat" || info.category == "movement" || info.category == "quest" ||
                info.category == "loot" || info.category == "social")
                value += ",fresh_world_state";
            if (AuthorityFor(info.name) == ActionAuthority::OwnerCommand)
                value += ",verified_owner_command";
            return value;
        }

        std::vector<ActionInfo> BuildCurated()
        {
            std::vector<ActionInfo> actions = {
                // Movement
                { "move_to",             "movement", "x,y,z,[range]",        "Walk to exact world coordinates; finishes when inside range.", "go", false },
                { "go_to",               "movement", "guid|name,[range]",    "Walk to an entity from the surroundings list (resolves its position).", "go", false },
                { "follow",              "movement", "",                     "Follow the master with full pathfinding (releases companion control to playerbots follow behaviour).", "follow", false },
                { "stay",                "movement", "",                     "Halt and hold position.", "stay", false },
                { "stop",                "movement", "",                     "Alias of stay: halt movement.", "stay", false },
                { "mount",               "movement", "",                     "Mount up when possible.", "mount", false },
                { "dismount",            "movement", "",                     "Dismount.", "", false },
                { "flee",                "movement", "",                     "Flee from the current attacker.", "flee", false },
                { "runaway",             "movement", "",                     "Kite mob away from danger.", "runaway", false },
                { "grind",               "movement", "",                     "Hunt nearby hostile mobs autonomously (hand behaviour back to playerbots).", "grind", false },
                { "wander",              "movement", "",                     "Random idle roam around the current spot.", "move random", false },
                { "travel_to",           "movement", "destination",          "Use playerbots travel to a named destination.", "go", false },
                { "summon",              "movement", "",                     "Summon bot to master's position.", "summon", false },
                { "disperse",            "movement", "[distance]",           "Maintain distance of X yards between bots.", "disperse set", false },
                { "disperse_disable",    "movement", "",                     "Reset disperse distance to default.", "disperse disable", false },

                // Combat
                { "attack",              "combat", "guid|name|master_target","Engage a target: sets the target, switches to combat and assists.", "attack", false },
                { "attack_my_target",    "combat", "",                       "Overrides current actions and attacks master's target immediately.", "do attack my target", false },
                { "assist",              "combat", "[role]",                 "Combat assist: role = dps|tank|aoe (default from bot spec).", "dps assist", false },
                { "aoe",                 "combat", "",                       "Area damage assist.", "dps aoe", false },
                { "pull",                "combat", "",                       "Pull the current target.", "pull my target", false },
                { "pull_back",           "combat", "",                       "Tank pulls mob with ranged skill and returns to starting point.", "pull back", false },
                { "mark_rti",            "combat", "",                       "Mark lowest health combat attacker with raid target icon.", "mark rti", false },
                { "behind",              "combat", "",                       "Move behind target's back (rear flank).", "behind", false },
                { "tank_face",           "combat", "",                       "Face target away from ranged group members.", "tank face", false },
                { "focus",               "combat", "",                       "Stop AoE/multi-target debuffs and focus on single target.", "focus", false },
                { "threat",              "combat", "",                       "Actively manage threat to avoid pulling aggro from tank.", "threat", false },
                { "boost",               "combat", "",                       "Use major burst and cooldown abilities.", "boost", false },
                { "cc",                  "combat", "",                       "Use crowd control ability on marked RTI target.", "cc", false },
                { "cast",                "combat", "spellid|spell,[guid|target]", "Cast a learned spell, optionally on a target.", "", false },
                { "cast_on",             "combat", "spell,target",           "Cast a named spell on a specific player/friendly target.", "cast", false },
                { "spell_exclude",       "combat", "action,[spellid]",       "Manage excluded spells list (+id, -id, reset).", "ss", false },
                { "pet_attack",          "combat", "",                       "Send the pet at the current target.", "pet attack", false },
                { "use_trinket",         "combat", "",                       "Use the best equipped trinket.", "use trinket", false },
                { "racial",              "combat", "",                       "Use the racial combat ability.", "", false },

                // Strategy
                { "strategy",            "strategy", "add|remove|spec,[state]", "Change playerbots strategies, e.g. add ['+grind'] remove ['-stay'].", "", true },
                { "set_action_mode",     "strategy", "mode",                  "Set the cognitive posture: combat, travel, idle or social.", "", false },
                { "pet_summon",          "strategy", "pet",                  "Select active pet (imp, voidwalker, succubus, felhunter, felguard).", "co", false },
                { "soulstone",           "strategy", "target",               "Use soulstone on target (master, self, tank, healer).", "ss", false },

                // NPC and quests
                { "talk_to",             "quest", "guid|name,[kind]",        "Walk to an NPC and talk (gossip, vendor, trainer, innkeeper).", "talk", false },
                { "interact",            "quest", "guid",                    "Walk to and use a creature or game object.", "", false },
                { "use_object",          "quest", "guid",                    "Use a game object (chest, quest object, door, lever).", "use", false },
                { "accept_quest",        "quest", "id",                      "Accept a quest from the current quest giver.", "accept quest", false },
                { "accept_all_quests",   "quest", "",                        "Accept every available quest from nearby quest givers.", "accept all quests", false },
                { "turn_in_quest",       "quest", "id",                      "Turn in a completed quest and take the reward.", "talk to quest giver", false },
                { "choose_reward",       "quest", "item",                    "Choose quest reward by item link or name.", "r", false },
                { "share_quest",         "quest", "id",                      "Share a quest with the group.", "share", false },
                { "drop_quest",          "quest", "id",                      "Abandon a quest.", "drop", false },
                { "quest_summary",       "quest", "[all]",                   "Show quest log summary or list all quests with links.", "quests", false },
                { "trainer",             "quest", "",                        "Train new class abilities at a nearby trainer.", "trainer", false },
                { "trainer_learn",       "quest", "",                        "Learn available abilities from the selected trainer.", "trainer learn", false },
                { "talents",             "quest", "",                        "Auto-assign talent points.", "auto talents", false },
                { "rpg_status",          "rpg", "state,[id]",                "Set RPG state (idle, rest, wander_random, wander_npc, go_grind, go_camp, travel_flight, outdoor_pvp, do_quest).", "rpg status", false },
                { "rpg_do_quest",        "rpg", "id",                        "Switch to RPG quest state on any valid quest ID or link.", "rpg do quest", false },

                // Loot and gathering
                { "loot",                "loot", "",                         "Loot the nearest lootable corpse.", "loot", false },
                { "loot_all",            "loot", "",                         "Enable auto loot and loot everything reachable.", "add all loot", false },
                { "loot_nearest",        "loot", "[radius]",                 "Walk to and loot the nearest corpse within radius (default 20y).", "loot", false },
                { "gather",              "loot", "",                         "Gather nearby herbs, veins and other resource nodes.", "add gathering loot", false },
                { "open_loot",           "loot", "",                         "Open the current loot window.", "open loot", false },
                { "loot_filter",         "loot", "filter,[item]",            "Set loot filter (all, normal, gray, quest, skill) or add/remove item.", "ll", false },

                // Economy and gear
                { "sell",                "economy", "[item|all_junk]",       "Sell grey junk, or a named item, to a nearby vendor.", "sell", false },
                { "buy",                 "economy", "item,[count]",          "Buy an item from a nearby vendor.", "buy", false },
                { "repair",              "economy", "",                      "Repair all equipment at a nearby repair vendor.", "repair", false },
                { "bank",                "economy", "[action],[item]",       "Visit the bank and deposit/withdraw as configured.", "bank", false },
                { "guild_bank",          "economy", "action,item",           "Deposit or withdraw items from guild bank.", "gb", false },
                { "mail",                "economy", "",                      "Check and handle mail.", "mail", false },
                { "send_mail",           "economy", "item|money",          "Send one item or money to the master through a nearby mailbox.", "sendmail", false },
                { "craft",               "economy", "item,[count]",          "Craft a known recipe through playerbots.", "craft", false },
                { "taxi",                "movement", "destination",           "Take a known flight path through playerbots.", "taxi", false },
                { "equip",               "economy", "item",                  "Equip an item by name or id.", "equip", false },
                { "unequip",             "economy", "item|slot",             "Unequip an item by name or equipment slot.", "ue", false },
                { "equip_upgrades",      "economy", "",                      "Auto-equip upgrades found in bags.", "equip upgrade", false },
                { "open_items",          "economy", "",                      "Open items in inventory that have loot (satchels, boxes).", "open items", false },
                { "use_item",            "economy", "item",                  "Use an item from inventory.", "u", false },
                { "use_item_on",         "economy", "item,target",           "Use an item on a target (e.g. gem on item).", "u", false },
                { "destroy_item",        "economy", "item",                  "Destroy an item from inventory.", "destroy", false },
                { "give_gold",           "economy", "gold,[silver],[copper]","Give gold to the master.", "gold", false },
                { "outfit",              "economy", "name,action,[item]",    "Manage outfits (equip, replace, update, reset, add, remove).", "outfit", false },
                { "maintenance",         "economy", "",                      "Run full playerbots maintenance (gear, ammo, reagents).", "maintenance", false },

                // Survival
                { "food",                "survival", "",                     "Eat food to restore health.", "food", false },
                { "drink",               "survival", "",                     "Drink to restore mana.", "drink", false },
                { "eat_drink",           "survival", "",                     "Sit and both eat and drink.", "food", false },
                { "tavern_rest",         "survival", "",                     "Living downtime: sit in inn/city, drink or rest.", "food", false },
                { "campfire_cook",       "survival", "",                     "Living downtime: pitch campfire and rest/cook in wilderness.", "", false },
                { "healing_potion",      "survival", "",                     "Use a healing potion.", "healing potion", false },
                { "mana_potion",         "survival", "",                     "Use a mana potion.", "mana potion", false },
                { "healthstone",         "survival", "",                     "Use a healthstone.", "healthstone", false },
                { "hearthstone",         "survival", "",                     "Use the hearthstone.", "hearthstone", false },
                { "revive",              "survival", "",                     "Recover the corpse / accept resurrection after death.", "revive from corpse", false },
                { "release",             "survival", "",                     "Release the spirit after death.", "auto release", false },

                // Social
                { "emote",               "social", "emote",                  "Play a body-language emote (wave, nod, salute, point, ...).", "", false },
                { "say",                 "social", "text,[channel]",         "Speak in game (only when the speech fallback is enabled).", "say", false },
                { "greet",               "social", "",                       "Greet nearby players/bots with an emote or line.", "greet", false },
                { "invite",              "social", "[name]",                 "Invite a nearby player to the group.", "invite nearby", false },
                { "leave_group",         "social", "",                       "Leave the current group.", "leave", false },
                { "give_leader",         "social", "",                       "Pass group/raid leader to master.", "give leader", false },
                { "lfg",                 "social", "[size]",                 "Join LFG queue or fill party/raid (size = 5|10|20|25|40).", "lfg", false },
                { "ready",               "social", "",                       "Answer a ready check.", "ready", false },
                { "duel_start",          "social", "guid|name",              "Challenge a player to a duel.", "", false },
                { "duel_accept",         "social", "",                       "Accept a pending duel.", "accept duel", false },
                { "duel_decline",        "social", "",                       "Decline a pending duel.", "", false },
                { "attack_duel_opponent","social", "guid",                   "Fight the duel opponent.", "attack duel opponent", false },
                { "trade",               "social", "[guid|name|target]",     "Initiate trade with target or master.", "", false },
                { "trade_start",         "social", "[guid|name|target]",     "Initiate trade with target or master.", "", false },
                { "trade_accept",        "social", "",                       "Accept the current trade.", "accept trade", false },
                { "trade_cancel",        "social", "",                       "Cancel the current trade.", "", false },
                { "trade_set_item",      "social", "item,[count],[slot]",    "Place or right-click an item into the active trade window.", "", false },
                { "trade_clear_item",    "social", "[item],[slot]",          "Remove an item from the active trade window.", "", false },
                { "trade_set_gold",      "social", "gold|copper",            "Adjust the gold offered in the active trade window.", "", false },
                { "trade_link_items",    "social", "[category]",             "Link tradeable inventory items into chat.", "", false },
                { "inspect",             "social", "[guid|name|target]",     "Inspect target or master.", "", false },
                { "target",              "social", "guid|name",              "Set selection target by name or guid.", "", false },
                { "pvp",                 "social", "",                       "Toggle PvP flag through the native playerbot command surface.", "flag pvp toggle", false },
                { "roll",                "social", "[item]",                 "Roll on the pending loot or linked item.", "roll", false },

                // Meta
                { "playerbot_action",    "meta", "action,[param]",           "Escape hatch: run ANY action registered by mod-playerbots (validated against the bot's own action list).", "", true },
                { "playerbot_command",   "meta", "command",                  "Escape hatch: send a raw playerbot chat command (separator aware, denylist filtered).", "", true },
                { "wait",                "meta", "seconds",                  "Pause the plan for N seconds.", "delay", false },
            };

            static std::string const resultSchema =
                "{\"type\":\"object\",\"required\":[\"api_version\",\"capability\",\"status\",\"verified\"],"
                "\"properties\":{\"api_version\":{\"type\":\"integer\"},"
                "\"capability\":{\"type\":\"string\"},\"status\":{\"type\":\"string\"},"
                "\"verified\":{\"type\":\"boolean\"},\"effects\":{\"type\":\"object\"},"
                "\"error\":{\"type\":[\"object\",\"null\"]}}}";

            for (ActionInfo& info : actions)
            {
                info.paramsJsonSchema = ParamsSchema(info.params);
                info.resultJsonSchema = resultSchema;
                info.bindingKind = BindingFor(info);
                info.authority = AuthorityName(AuthorityFor(info.name));
                info.preconditions = PreconditionsFor(info);
                info.completionPolicy = CompletionFor(info.name);
            }
            return actions;
        }
    }

    std::vector<ActionInfo> const& Curated()
    {
        static std::vector<ActionInfo> const actions = BuildCurated();
        return actions;
    }

    ActionInfo const* Find(std::string const& name)
    {
        static std::unordered_map<std::string, ActionInfo const*> const map = []() {
            std::unordered_map<std::string, ActionInfo const*> m;
            for (ActionInfo const& info : Curated())
                m[info.name] = &info;
            return m;
        }();

        auto it = map.find(name);
        return it != map.end() ? it->second : nullptr;
    }

    bool IsCurated(std::string const& name)
    {
        return Find(name) != nullptr;
    }

    ActionAuthority RequiredAuthority(std::string const& name)
    {
        return AuthorityFor(name);
    }

    char const* AuthorityName(ActionAuthority authority)
    {
        switch (authority)
        {
            case ActionAuthority::OwnerCommand: return "owner_command";
            case ActionAuthority::GmManual: return "gm_manual";
            default: return "autonomous";
        }
    }

    bool IsAuthorized(std::string const& name, std::string const& grantedAuthority,
                      uint32 originSourceGuid, uint32 masterGuid)
    {
        ActionAuthority required = RequiredAuthority(name);
        if (required == ActionAuthority::Autonomous)
            return true;
        if (grantedAuthority == "gm_manual")
            return true;
        return required == ActionAuthority::OwnerCommand && grantedAuthority == "owner_command" &&
               masterGuid != 0 && originSourceGuid == masterGuid;
    }

    std::string CuratedJson()
    {
        std::ostringstream ss;
        ss << "{\"actions\":[";
        std::vector<ActionInfo> const& actions = Curated();
        for (size_t i = 0; i < actions.size(); ++i)
        {
            if (i > 0)
                ss << ",";

            ss << "{\"name\":\"" << actions[i].name
               << "\",\"category\":\"" << actions[i].category
               << "\",\"params\":\"" << actions[i].params
               << "\",\"description\":\"" << actions[i].description
               << "\",\"playerbot_action\":\"" << actions[i].playerbotAction
               << "\",\"passthrough\":" << (actions[i].passthrough ? "true" : "false")
               << ",\"api_version\":" << (uint32)actions[i].apiVersion
               << ",\"params_schema_json\":" << actions[i].paramsJsonSchema
               << ",\"result_schema_json\":" << actions[i].resultJsonSchema
               << ",\"binding_kind\":\"" << actions[i].bindingKind
               << "\",\"authority\":\"" << actions[i].authority
               << "\",\"preconditions\":\"" << actions[i].preconditions
               << "\",\"completion_policy\":\"" << actions[i].completionPolicy << "\""
               << "}";
        }
        ss << "]}";
        return ss.str();
    }

    std::string CuratedPromptList()
    {
        std::ostringstream ss;
        std::string currentCategory;
        for (ActionInfo const& info : Curated())
        {
            if (info.category != currentCategory)
            {
                currentCategory = info.category;
                ss << "\n" << currentCategory << ": ";
            }
            else
            {
                ss << ", ";
            }

            ss << info.name << "(" << info.params << ")";
        }
        return ss.str();
    }
}
