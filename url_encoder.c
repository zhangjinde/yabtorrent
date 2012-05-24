
/*
 * Convert ascii byte streams into url encoded character streams as per rfc1738
 *
 *
 * http://en.wikipedia.org/wiki/Percent-encoding
 * http://www.faqs.org/rfcs/rfc1738.html
 *
 *
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Converts a hex character to its integer value */
char from_hex(
    char ch
)
{
    return isdigit(ch) ? ch - '0' : tolower(ch) - 'a' + 10;
}

/* Converts an integer value to its hex character*/
char to_hex(
    char code
)
{
    static char hex[] = "0123456789ABCDEF";

    return hex[code & 15];
}

/* Returns a url-encoded version of str */
/* IMPORTANT: be sure to free() the returned string after use */
char *url_encode(
    const char *str
)
{
    const char *pstr = str;

    char *buf = malloc(strlen(str) * 3 + 1), *pbuf = buf;

    while (*pstr)
    {
        if (isalnum(*pstr))     // || *pstr == '-' || *pstr == '_' || *pstr == '.'
//            || *pstr == '~')
        {
            *pbuf++ = *pstr;
        }
//        else if (*pstr == ' ')
//        {
//            *pbuf++ = '+';
//        }
        else
        {
            *pbuf++ = '%', *pbuf++ = to_hex(*pstr >> 4), *pbuf++ =
                to_hex(*pstr & 15);
        }
        pstr++;
    }
    *pbuf = '\0';
    return buf;
}

/* Returns a url-decoded version of str */
/* IMPORTANT: be sure to free() the returned string after use */
char *url_decode(
    const char *str
)
{
    const char *pstr = str;

    char *buf = malloc(strlen(str) + 1), *pbuf = buf;

    while (*pstr)
    {
        if (*pstr == '%')
        {
            if (pstr[1] && pstr[2])
            {
                *pbuf++ = from_hex(pstr[1]) << 4 | from_hex(pstr[2]);
                pstr += 2;
            }
        }
        else if (*pstr == '+')
        {
            *pbuf++ = ' ';
        }
        else
        {
            *pbuf++ = *pstr;
        }
        pstr++;
    }
    *pbuf = '\0';
    return buf;
}