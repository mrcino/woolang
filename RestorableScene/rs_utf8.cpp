#include "rs_utf8.hpp"
#include <cwchar>
#include <cstring>

namespace rs
{
    uint8_t u8chsize(const char* chidx)
    {
        std::mbstate_t mb = {};

        auto strlength = strlen(chidx);

        if (std::mbrlen(chidx, 1, &mb) == -2)
        {
            if (strlength)
            {
                uint8_t strsz = strlength > UINT8_MAX ? UINT8_MAX : (uint8_t)strlength;
                return (uint8_t)std::mbrlen(chidx + 1, strsz - 1, &mb) + 1;
            }

        }
        return 1;
    }
    size_t u8strlen(rs_string_t u8str)
    {
        size_t strlength = 0;
        while (*u8str)
        {
            strlength++;
            u8str += u8chsize(u8str);
        }
        return strlength;
    }
    rs_string_t u8stridxstr(rs_string_t u8str, size_t chidx)
    {
        while (chidx && *u8str)
        {
            --chidx;
            u8str += u8chsize(u8str);
        }
        return u8str;
    }
    size_t u8stridx(rs_string_t u8str, size_t chidx)
    {
        return u8stridxstr(u8str, chidx) - u8str;
    }
    rs_string_t u8substr(rs_string_t u8str, size_t from, size_t length, size_t* out_sub_len)
    {
        auto substr = u8stridxstr(u8str, from);
        auto end_place = u8stridxstr(u8str, from + length);
        *out_sub_len = end_place - substr;
        return substr;
    }
}