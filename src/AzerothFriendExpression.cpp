#include "AzerothFriendExpression.h"
#include "AzerothFriendConfig.h"
#include "AzerothFriendUtilities.h"
#include "DBCStores.h"
#include "Log.h"
#include "MotionMaster.h"
#include "UnitDefines.h"
#include "ObjectAccessor.h"
#include "Opcodes.h"
#include "Player.h"
#include "Random.h"
#include "SharedDefines.h"
#include "WorldPacket.h"

#if __has_include("Playerbots.h")
    #include "Playerbots.h"
    #include "PlayerbotAI.h"
    #include "PlayerbotMgr.h"
    #define AF_HAS_PLAYERBOTS 1
#else
    #define AF_HAS_PLAYERBOTS 0
#endif

#include <algorithm>
#include <cctype>
#include <vector>
#include <string>
#include <unordered_map>

namespace
{
    bool IsBotPlayer(Player* player)
    {
        if (!player)
            return false;
        if (player->GetSession() && player->GetSession()->IsHeadless())
            return true;
#if AF_HAS_PLAYERBOTS
        return PlayerbotsMgr::instance().GetPlayerbotAI(player) != nullptr;
#else
        return false;
#endif
    }

    std::string ToLower(std::string v)
    {
        for (char& c : v)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return v;
    }

    std::string NormalizeEmoteWord(const std::string& raw)
    {
        std::string s = ToLower(raw);
        std::string letters;
        letters.reserve(s.size());
        for (char c : s)
            if (std::isalpha(static_cast<unsigned char>(c)))
                letters.push_back(c);

        if (letters.size() > 4 && letters.compare(letters.size() - 3, 3, "ing") == 0)
            return letters.substr(0, letters.size() - 3);
        if (letters.size() > 3 && letters.back() == 's')
            return letters.substr(0, letters.size() - 1);

        return letters;
    }

    const std::unordered_map<std::string, uint32_t>& EmoteNameTable()
    {
        static const std::unordered_map<std::string, uint32_t> table = {
            // Greetings and courtesy
            { "wave", TEXT_EMOTE_WAVE },           { "hello", TEXT_EMOTE_HELLO },
            { "hi", TEXT_EMOTE_HELLO },            { "greet", TEXT_EMOTE_GREET },
            { "hail", TEXT_EMOTE_HAIL },           { "welcome", TEXT_EMOTE_WELCOME },
            { "bow", TEXT_EMOTE_BOW },             { "curtsey", TEXT_EMOTE_CURTSEY },
            { "salute", TEXT_EMOTE_SALUTE },       { "thank", TEXT_EMOTE_THANK },
            { "bye", TEXT_EMOTE_BYE },             { "farewell", TEXT_EMOTE_BYE },
            { "introduce", TEXT_EMOTE_INTRODUCE }, { "congratulate", TEXT_EMOTE_CONGRATULATE },
            { "grats", TEXT_EMOTE_CONGRATULATE },  { "praise", TEXT_EMOTE_PRAISE },
            { "commend", TEXT_EMOTE_COMMEND },     { "highfive", TEXT_EMOTE_HIGHFIVE },
            { "goodluck", TEXT_EMOTE_GOODLUCK },   { "encourage", TEXT_EMOTE_ENCOURAGE },
            { "toast", TEXT_EMOTE_TOAST },

            // Agreement / disagreement
            { "nod", TEXT_EMOTE_NOD },             { "agree", TEXT_EMOTE_AGREE },
            { "yes", TEXT_EMOTE_AGREE },           { "no", TEXT_EMOTE_NO },
            { "disagree", TEXT_EMOTE_DISAGREE },   { "shake", TEXT_EMOTE_SHAKE },
            { "shrug", TEXT_EMOTE_SHRUG },         { "veto", TEXT_EMOTE_VETO },
            { "doubt", TEXT_EMOTE_DOUBT },         { "ready", TEXT_EMOTE_READY },

            // Amusement
            { "laugh", TEXT_EMOTE_LAUGH },         { "chuckle", TEXT_EMOTE_CHUCKLE },
            { "giggle", TEXT_EMOTE_GIGGLE },       { "cackle", TEXT_EMOTE_CACKLE },
            { "guffaw", TEXT_EMOTE_GUFFAW },       { "rofl", TEXT_EMOTE_ROFL },
            { "snicker", TEXT_EMOTE_SNICKER },     { "smirk", TEXT_EMOTE_SMIRK },
            { "grin", TEXT_EMOTE_GRIN },           { "smile", TEXT_EMOTE_SMILE },
            { "joke", TEXT_EMOTE_JOKE },           { "jk", TEXT_EMOTE_JK },
            { "tease", TEXT_EMOTE_TEASE },         { "wink", TEXT_EMOTE_WINK },
            { "happy", TEXT_EMOTE_HAPPY },         { "cheer", TEXT_EMOTE_CHEER },
            { "applaud", TEXT_EMOTE_APPLAUD },     { "clap", TEXT_EMOTE_CLAP },
            { "golfclap", TEXT_EMOTE_GOLFCLAP },   { "dance", TEXT_EMOTE_DANCE },
            { "flex", TEXT_EMOTE_FLEX },           { "victory", TEXT_EMOTE_VICTORY },
            { "gloat", TEXT_EMOTE_GLOAT },

            // Distress and sorrow
            { "cry", TEXT_EMOTE_CRY },             { "sigh", TEXT_EMOTE_SIGH },
            { "mourn", TEXT_EMOTE_MOURN },         { "groan", TEXT_EMOTE_GROAN },
            { "whine", TEXT_EMOTE_WHINE },         { "moan", TEXT_EMOTE_MOAN },
            { "panic", TEXT_EMOTE_PANIC },         { "cower", TEXT_EMOTE_COWER },
            { "cringe", TEXT_EMOTE_CRINGE },       { "plead", TEXT_EMOTE_PLEAD },
            { "beg", TEXT_EMOTE_BEG },             { "grovel", TEXT_EMOTE_GROVEL },
            { "apologize", TEXT_EMOTE_APOLOGIZE }, { "sorry", TEXT_EMOTE_APOLOGIZE },
            { "comfort", TEXT_EMOTE_COMFORT },     { "soothe", TEXT_EMOTE_SOOTHE },
            { "pity", TEXT_EMOTE_PITY },           { "scared", TEXT_EMOTE_SCARED },
            { "surrender", TEXT_EMOTE_SURRENDER }, { "helpme", TEXT_EMOTE_HELPME },
            { "faint", TEXT_EMOTE_FAINT },         { "facepalm", TEXT_EMOTE_FACEPALM },
            { "blame", TEXT_EMOTE_BLAME },         { "fail", TEXT_EMOTE_FAIL },
            { "embarrass", TEXT_EMOTE_EMBARRASS }, { "blush", TEXT_EMOTE_BLUSH },
            { "bashful", TEXT_EMOTE_BASHFUL },     { "shy", TEXT_EMOTE_SHY },

            // Hostility
            { "angry", TEXT_EMOTE_ANGRY },         { "glare", TEXT_EMOTE_GLARE },
            { "growl", TEXT_EMOTE_GROWL },         { "snarl", TEXT_EMOTE_SNARL },
            { "roar", TEXT_EMOTE_ROAR },           { "threaten", TEXT_EMOTE_THREATEN },
            { "taunt", TEXT_EMOTE_TAUNT },         { "mock", TEXT_EMOTE_MOCK },
            { "insult", TEXT_EMOTE_INSULT },       { "rude", TEXT_EMOTE_RUDE },
            { "spit", TEXT_EMOTE_SPIT },           { "snub", TEXT_EMOTE_SNUB },
            { "challenge", TEXT_EMOTE_CHALLENGE }, { "brandish", TEXT_EMOTE_BRANDISH },
            { "enemy", TEXT_EMOTE_ENEMY },         { "charge", TEXT_EMOTE_CHARGE },
            { "slap", TEXT_EMOTE_SLAP },           { "shoo", TEXT_EMOTE_SHOO },
            { "stare", TEXT_EMOTE_STARE },         { "frown", TEXT_EMOTE_FROWN },
            { "glower", TEXT_EMOTE_GLOWER },

            // Thought and attention
            { "ponder", TEXT_EMOTE_PONDER },       { "think", TEXT_EMOTE_PONDER },
            { "confused", TEXT_EMOTE_CONFUSED },   { "puzzle", TEXT_EMOTE_PUZZLE },
            { "boggle", TEXT_EMOTE_BOGGLE },       { "curious", TEXT_EMOTE_CURIOUS },
            { "peer", TEXT_EMOTE_PEER },           { "listen", TEXT_EMOTE_LISTEN },
            { "point", TEXT_EMOTE_POINT },         { "beckon", TEXT_EMOTE_BECKON },
            { "talk", TEXT_EMOTE_TALK },           { "amaze", TEXT_EMOTE_AMAZE },
            { "awe", TEXT_EMOTE_AWE },             { "gasp", TEXT_EMOTE_GASP },
            { "surprised", TEXT_EMOTE_SURPRISED }, { "eyebrow", TEXT_EMOTE_EYEBROW },
            { "lost", TEXT_EMOTE_LOST },           { "serious", TEXT_EMOTE_SERIOUS },
            { "badfeeling", TEXT_EMOTE_BADFEELING },

            // States and needs
            { "tired", TEXT_EMOTE_TIRED },         { "yawn", TEXT_EMOTE_YAWN },
            { "sleep", TEXT_EMOTE_SLEEP },         { "bored", TEXT_EMOTE_BORED },
            { "hungry", TEXT_EMOTE_HUNGRY },       { "thirsty", TEXT_EMOTE_THIRSTY },
            { "cold", TEXT_EMOTE_COLD },           { "drink", TEXT_EMOTE_DRINK },
            { "eat", TEXT_EMOTE_EAT },             { "oom", TEXT_EMOTE_OOM },
            { "healme", TEXT_EMOTE_HEALME },       { "work", TEXT_EMOTE_WORK },
            { "pray", TEXT_EMOTE_PRAY },           { "kneel", TEXT_EMOTE_KNEEL },
            { "stand", TEXT_EMOTE_STAND },         { "sit", TEXT_EMOTE_SIT },
            { "calm", TEXT_EMOTE_CALM },           { "brb", TEXT_EMOTE_BRB },
            { "wait", TEXT_EMOTE_WAIT },           { "follow", TEXT_EMOTE_FOLLOW },
            { "flee", TEXT_EMOTE_FLEE },           { "incoming", TEXT_EMOTE_INCOMING },
            { "cough", TEXT_EMOTE_COUGH },         { "whistle", TEXT_EMOTE_WHISTLE },
            { "shiver", TEXT_EMOTE_SHIVER },       { "scratch", TEXT_EMOTE_SCRATCH },
            { "crack", TEXT_EMOTE_CRACK },         { "raise", TEXT_EMOTE_RAISE },
            { "ding", TEXT_EMOTE_DING },

            // Attitude
            { "rolleyes", TEXT_EMOTE_ROLLEYES },   { "eyeroll", TEXT_EMOTE_ROLLEYES },
            { "scoff", TEXT_EMOTE_SCOFF },         { "snort", TEXT_EMOTE_SNORT },
            { "grunt", TEXT_EMOTE_SNORT },         { "scowl", TEXT_EMOTE_SCOWL },
            { "pout", TEXT_EMOTE_POUT },           { "crossarms", TEXT_EMOTE_CROSSARMS },
            { "foldarms", TEXT_EMOTE_CROSSARMS },  { "shakefist", TEXT_EMOTE_SHAKEFIST },
            { "warn", TEXT_EMOTE_WARN },           { "scold", TEXT_EMOTE_SCOLD }
        };
        return table;
    }

    uint8 StandStateForEmoteAnim(uint32 anim)
    {
        switch (anim)
        {
            case EMOTE_STATE_SIT:
            case EMOTE_STATE_SIT_CHAIR_MED:
            case EMOTE_STATE_SIT_CHAIR_HIGH:
            case EMOTE_STATE_SIT_CHAIR_LOW:
                return UNIT_STAND_STATE_SIT;
            case EMOTE_STATE_SLEEP:
                return UNIT_STAND_STATE_SLEEP;
            case EMOTE_STATE_KNEEL:
                return UNIT_STAND_STATE_KNEEL;
            default:
                return UNIT_STAND_STATE_STAND;
        }
    }

    class BotClearEmoteStateEvent : public BasicEvent
    {
    public:
        BotClearEmoteStateEvent(ObjectGuid guid, uint8 stand, uint32 state)
            : m_guid(guid), m_stand(stand), m_state(state) { }

        bool Execute(uint64 /*e_time*/, uint32 /*p_time*/) override
        {
            Player* bot = ObjectAccessor::FindConnectedPlayer(m_guid);
            if (!bot || !bot->IsInWorld() || !bot->IsAlive())
                return true;

            if (m_stand != UNIT_STAND_STATE_STAND && bot->getStandState() == m_stand)
                bot->SetStandState(UNIT_STAND_STATE_STAND);

            if (m_state != 0 && bot->GetUInt32Value(UNIT_NPC_EMOTESTATE) == m_state)
                bot->SetUInt32Value(UNIT_NPC_EMOTESTATE, 0);

            return true;
        }

    private:
        ObjectGuid m_guid;
        uint8      m_stand;
        uint32     m_state;
    };

    class BotExpressionEvent : public BasicEvent
    {
    public:
        BotExpressionEvent(ObjectGuid guid, ObjectGuid target, uint32 emote)
            : m_guid(guid), m_target(target), m_emote(emote) { }

        bool Execute(uint64 /*e_time*/, uint32 /*p_time*/) override
        {
            Player* bot = ObjectAccessor::FindConnectedPlayer(m_guid);
            if (!bot || !bot->IsInWorld() || !bot->IsAlive())
                return true;

            if (!m_target.IsEmpty())
            {
                if (Player* target = ObjectAccessor::FindConnectedPlayer(m_target))
                {
                    if (IsSafeForFacing(bot))
                    {
                        bot->SetFacingToObject(target);
                        bot->SetOrientation(bot->GetAngle(target));
                    }
                }
            }

            if (m_emote != 0)
                BotPlayTextEmote(bot, m_emote, m_target);

            return true;
        }

    private:
        ObjectGuid m_guid;
        ObjectGuid m_target;
        uint32     m_emote;
    };
}

uint32_t LookupTextEmoteId(const std::string& name)
{
    const std::string key = NormalizeEmoteWord(name);
    if (key.empty())
        return 0;

    const auto& table = EmoteNameTable();
    auto it = table.find(key);
    return it == table.end() ? 0u : it->second;
}

std::string LookupTextEmoteName(uint32_t id)
{
    if (!id)
        return "";

    for (const auto& [name, value] : EmoteNameTable())
        if (value == id)
            return name;

    return "";
}

uint32_t ExtractEmoteTag(std::string& response)
{
    uint32_t found = 0;
    static const std::string open = "[emote:";
    for (;;)
    {
        size_t start = response.find(open);
        if (start == std::string::npos)
            break;
        size_t close = response.find(']', start);
        if (close == std::string::npos)
        {
            response.erase(start);
            break;
        }

        const size_t nameStart = start + open.size();
        std::string word = response.substr(nameStart, close - nameStart);
        if (!found)
            found = LookupTextEmoteId(word);
        response.erase(start, close - start + 1);
    }

    for (;;)
    {
        size_t start = response.find('*');
        if (start == std::string::npos)
            break;
        size_t close = response.find('*', start + 1);
        if (close == std::string::npos)
        {
            response.erase(start, 1);
            break;
        }

        std::string word = response.substr(start + 1, close - start - 1);
        uint32_t em = LookupTextEmoteId(word);
        if (em)
        {
            if (!found)
                found = em;
            response.erase(start, close - start + 1);
        }
        else
            break;
    }

    while (!response.empty() && std::isspace(static_cast<unsigned char>(response.front())))
        response.erase(response.begin());
    while (!response.empty() && std::isspace(static_cast<unsigned char>(response.back())))
        response.pop_back();

    return found;
}

std::string Expression_BuildGesturePrompt()
{
    return " You may end your reply with a single gesture tag such as [emote:wave], [emote:nod], [emote:shrug] when one genuinely fits.";
}

bool IsSafeForFacing(Player* bot)
{
    if (!bot || !bot->IsInWorld() || !bot->IsAlive())
        return false;
    if (!bot->IsStopped() || bot->IsInCombat())
        return false;
    if (bot->IsInFlight() || bot->GetTransport())
        return false;
    if (bot->IsBeingTeleported())
        return false;
    if (bot->IsNonMeleeSpellCast(false))
        return false;

    MotionMaster* mm = bot->GetMotionMaster();
    return mm && mm->GetMotionSlotType(MOTION_SLOT_CONTROLLED) == NULL_MOTION_TYPE;
}

void BotPlayTextEmote(Player* bot, uint32_t textEmoteId, ObjectGuid targetGuid)
{
    if (!bot || !textEmoteId || !bot->IsInWorld() || !bot->IsAlive())
        return;

    WorldSession* session = bot->GetSession();
    if (!session)
        return;

    WorldPacket data(CMSG_TEXT_EMOTE, 4 + 4 + 8);
    data << uint32(textEmoteId);
    data << uint32(0);
    data << targetGuid;

    session->HandleTextEmoteOpcode(data);

    uint8 appliedStand = UNIT_STAND_STATE_STAND;
    if (EmotesTextEntry const* em = sEmotesTextStore.LookupEntry(textEmoteId))
    {
        const uint8 wanted = StandStateForEmoteAnim(em->textid);
        if (wanted != UNIT_STAND_STATE_STAND && bot->getStandState() == UNIT_STAND_STATE_STAND)
        {
            bot->SetStandState(wanted);
            appliedStand = wanted;
        }
    }

    const uint32 emoteState = bot->GetUInt32Value(UNIT_NPC_EMOTESTATE);
    if (emoteState != 0 || appliedStand != UNIT_STAND_STATE_STAND)
    {
        bot->m_Events.AddEvent(
            new BotClearEmoteStateEvent(bot->GetGUID(), appliedStand, emoteState),
            bot->m_Events.CalculateTime(3000));
    }
}

void ScheduleBotExpression(Player* bot, ObjectGuid targetGuid, uint32_t textEmoteId, uint32_t delayMs)
{
    if (!bot)
        return;

    bot->m_Events.AddEvent(
        new BotExpressionEvent(bot->GetGUID(), targetGuid, textEmoteId),
        bot->m_Events.CalculateTime(delayMs));
}

uint32_t GetMirrorEmote(uint32_t inbound)
{
    switch (inbound)
    {
        case TEXT_EMOTE_WAVE:         return TEXT_EMOTE_WAVE;
        case TEXT_EMOTE_HELLO:        return TEXT_EMOTE_HELLO;
        case TEXT_EMOTE_GREET:        return TEXT_EMOTE_GREET;
        case TEXT_EMOTE_HAIL:         return TEXT_EMOTE_WAVE;
        case TEXT_EMOTE_BOW:          return TEXT_EMOTE_BOW;
        case TEXT_EMOTE_CURTSEY:      return TEXT_EMOTE_BOW;
        case TEXT_EMOTE_SALUTE:       return TEXT_EMOTE_SALUTE;
        case TEXT_EMOTE_NOD:          return TEXT_EMOTE_NOD;
        case TEXT_EMOTE_AGREE:        return TEXT_EMOTE_AGREE;
        case TEXT_EMOTE_CHEER:        return TEXT_EMOTE_CHEER;
        case TEXT_EMOTE_APPLAUD:      return TEXT_EMOTE_CLAP;
        case TEXT_EMOTE_CLAP:         return TEXT_EMOTE_CLAP;
        case TEXT_EMOTE_LAUGH:        return TEXT_EMOTE_CHUCKLE;
        case TEXT_EMOTE_DANCE:        return TEXT_EMOTE_DANCE;
        case TEXT_EMOTE_THANK:        return TEXT_EMOTE_BOW;
        case TEXT_EMOTE_CONGRATULATE: return TEXT_EMOTE_THANK;
        case TEXT_EMOTE_HUG:          return TEXT_EMOTE_HUG;
        case TEXT_EMOTE_HIGHFIVE:     return TEXT_EMOTE_HIGHFIVE;
        case TEXT_EMOTE_TOAST:        return TEXT_EMOTE_TOAST;
        case TEXT_EMOTE_BYE:          return TEXT_EMOTE_WAVE;
        default:                      return 0;
    }
}

uint32_t GetCounterEmote(uint32_t inbound)
{
    switch (inbound)
    {
        case TEXT_EMOTE_FLEX:      return TEXT_EMOTE_LAUGH;
        case TEXT_EMOTE_RUDE:      return TEXT_EMOTE_GLARE;
        case TEXT_EMOTE_SPIT:      return TEXT_EMOTE_ANGRY;
        case TEXT_EMOTE_INSULT:    return TEXT_EMOTE_SNARL;
        case TEXT_EMOTE_MOCK:      return TEXT_EMOTE_FROWN;
        case TEXT_EMOTE_TAUNT:     return TEXT_EMOTE_THREATEN;
        case TEXT_EMOTE_THREATEN:  return TEXT_EMOTE_BRANDISH;
        case TEXT_EMOTE_CHALLENGE: return TEXT_EMOTE_READY;
        case TEXT_EMOTE_POKE:      return TEXT_EMOTE_EYEBROW;
        case TEXT_EMOTE_TICKLE:    return TEXT_EMOTE_GIGGLE;
        case TEXT_EMOTE_KISS:      return TEXT_EMOTE_BLUSH;
        case TEXT_EMOTE_FLIRT:     return TEXT_EMOTE_BASHFUL;
        case TEXT_EMOTE_CRY:       return TEXT_EMOTE_COMFORT;
        case TEXT_EMOTE_MOURN:     return TEXT_EMOTE_COMFORT;
        case TEXT_EMOTE_PLEAD:     return TEXT_EMOTE_PONDER;
        case TEXT_EMOTE_BEG:       return TEXT_EMOTE_SHRUG;
        case TEXT_EMOTE_HELPME:    return TEXT_EMOTE_READY;
        case TEXT_EMOTE_OOM:       return TEXT_EMOTE_NOD;
        case TEXT_EMOTE_PONDER:    return TEXT_EMOTE_SHRUG;
        case TEXT_EMOTE_STARE:     return TEXT_EMOTE_EYEBROW;
        case TEXT_EMOTE_ROAR:      return TEXT_EMOTE_CHEER;
        default:                   return 0;
    }
}

AzerothFriendEmoteScript::AzerothFriendEmoteScript() : PlayerScript("AzerothFriendEmoteScript", { PLAYERHOOK_ON_TEXT_EMOTE }) { }

void AzerothFriendEmoteScript::OnPlayerTextEmote(Player* player, uint32 textEmote, uint32 /*emoteNum*/, ObjectGuid guid)
{
    if (!player || guid.IsEmpty())
        return;

    if (IsBotPlayer(player))
        return;

    Player* bot = ObjectAccessor::FindConnectedPlayer(guid);
    if (!bot || bot == player || !bot->IsInWorld())
        return;

    if (!IsBotPlayer(bot))
        return;

    if (bot->IsInCombat() || player->GetMap() != bot->GetMap())
        return;

    if (bot->GetDistance(player) > 30.0f)
        return;

    uint32_t reactEmote = GetMirrorEmote(textEmote);
    if (!reactEmote)
        reactEmote = GetCounterEmote(textEmote);

    if (reactEmote)
    {
        ScheduleBotExpression(bot, player->GetGUID(), reactEmote, 500);
    }
}
