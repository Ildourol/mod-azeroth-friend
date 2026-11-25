#ifndef MOD_AZEROTH_FRIEND_UTILITIES_H
#define MOD_AZEROTH_FRIEND_UTILITIES_H

#include <string>
#include <vector>
#include <sstream>
#include <cctype>
#include <fmt/format.h>
#include "Log.h"

// Safe formatting utility for mod-azeroth-friend.
template<typename... Args>
inline std::string SafeFormat(const std::string& templ, Args&&... args)
{
    try
    {
        return fmt::vformat(templ, fmt::make_format_args(args...));
    }
    catch (const fmt::format_error& e)
    {
        LOG_ERROR("module.azerothfriend", "Format error: {} | Template: {}", e.what(), templ);
        return "[Format Error]";
    }
}

// Strip color codes and hyperlinks from class/role names
inline std::string CleanRoleForPrompt(const std::string& role)
{
    std::string out;
    out.reserve(role.size());

    for (size_t i = 0; i < role.size(); )
    {
        if (role[i] == '|' && i + 1 < role.size())
        {
            const char code = role[i + 1];

            if (code == 'c')
            {
                i += 2;
                for (size_t n = 0; n < 8 && i < role.size() &&
                                   std::isxdigit(static_cast<unsigned char>(role[i])); ++n)
                    ++i;
                continue;
            }

            if (code == 'r' || code == 'h' || code == 'H')
            {
                i += 2;
                continue;
            }
        }

        out += role[i++];
    }

    const size_t open = out.find('(');
    if (open != std::string::npos)
    {
        const size_t close = out.find(')', open);
        if (close != std::string::npos && close > open + 1)
        {
            bool numeric = true;
            for (size_t i = open + 1; i < close && numeric; ++i)
            {
                const char c = out[i];
                if (!std::isdigit(static_cast<unsigned char>(c)) && c != '/' && c != ' ')
                    numeric = false;
            }

            if (numeric)
                out.erase(open, close - open + 1);
        }
    }

    std::string collapsed;
    collapsed.reserve(out.size());
    bool prevSpace = false;
    for (char c : out)
    {
        const bool isSpace = (c == ' ' || c == '\t');
        if (isSpace && (prevSpace || collapsed.empty()))
            continue;

        collapsed += isSpace ? ' ' : c;
        prevSpace = isSpace;
    }

    while (!collapsed.empty() && collapsed.back() == ' ')
        collapsed.pop_back();

    return collapsed;
}

inline std::vector<std::string> SplitString(const std::string& str, char delim)
{
    std::vector<std::string> tokens;
    std::stringstream ss(str);
    std::string token;
    while (std::getline(ss, token, delim))
    {
        size_t start = token.find_first_not_of(" \t");
        size_t end = token.find_last_not_of(" \t");
        if (start != std::string::npos && end != std::string::npos)
            tokens.push_back(token.substr(start, end - start + 1));
    }
    return tokens;
}

inline std::string Utf8Truncate(std::string s, size_t max)
{
    if (s.size() <= max)
        return s;
    size_t cut = max;
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80)
        --cut;
    s.resize(cut);
    return s;
}

inline std::string SanitizeUTF8(const std::string& str)
{
    std::string result;
    result.reserve(str.size());
    for (size_t i = 0; i < str.size(); )
    {
        unsigned char c = static_cast<unsigned char>(str[i]);
        if (c <= 0x7F)
        {
            result.push_back(str[i]);
            i++;
        }
        else if ((c & 0xE0) == 0xC0)
        {
            if (i + 1 < str.size() && (static_cast<unsigned char>(str[i + 1]) & 0xC0) == 0x80)
            {
                result.push_back(str[i]);
                result.push_back(str[i + 1]);
                i += 2;
            }
            else
            {
                result.push_back(' ');
                i++;
            }
        }
        else if ((c & 0xF0) == 0xE0)
        {
            if (i + 2 < str.size() &&
                (static_cast<unsigned char>(str[i + 1]) & 0xC0) == 0x80 &&
                (static_cast<unsigned char>(str[i + 2]) & 0xC0) == 0x80)
            {
                result.push_back(str[i]);
                result.push_back(str[i + 1]);
                result.push_back(str[i + 2]);
                i += 3;
            }
            else
            {
                result.push_back(' ');
                i++;
            }
        }
        else if ((c & 0xF8) == 0xF0)
        {
            if (i + 3 < str.size() &&
                (static_cast<unsigned char>(str[i + 1]) & 0xC0) == 0x80 &&
                (static_cast<unsigned char>(str[i + 2]) & 0xC0) == 0x80 &&
                (static_cast<unsigned char>(str[i + 3]) & 0xC0) == 0x80)
            {
                result.push_back(str[i]);
                result.push_back(str[i + 1]);
                result.push_back(str[i + 2]);
                result.push_back(str[i + 3]);
                i += 4;
            }
            else
            {
                result.push_back(' ');
                i++;
            }
        }
        else
        {
            result.push_back(' ');
            i++;
        }
    }
    return result;
}

#endif // MOD_AZEROTH_FRIEND_UTILITIES_H

