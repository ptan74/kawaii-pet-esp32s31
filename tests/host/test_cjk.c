// SPDX-License-Identifier: Apache-2.0
/* CJK text survives cleaning; emoji still dropped; Chinese name extraction. */
#include <stdio.h>
#include <string.h>
#include "pet_text.h"
int main(void)
{
    char out[256];
    int bad = 0;
    pet_text_clean("汪汪！我好开心见到你，帕特里克。", out, sizeof(out));
    bad |= strstr(out, "汪汪") == NULL || strstr(out, "帕特里克") == NULL;
    pet_text_clean("멍멍! 만나서 반가워요 \xF0\x9F\x90\xB6", out, sizeof(out));
    bad |= strstr(out, "멍멍") == NULL || strstr(out, "\xF0\x9F") != NULL;
    pet_text_clean("こんにちは、パトリック", out, sizeof(out));
    bad |= strstr(out, "こんにちは") == NULL;
    pet_name_extract("你好小狗，我叫帕特里克。", out, sizeof(out));
    bad |= strcmp(out, "帕特里克") != 0;
    pet_name_extract("Hi, my name is patrick.", out, sizeof(out));
    bad |= strcmp(out, "Patrick") != 0;
    printf(bad ? "cjk: FAIL\n" : "cjk: all tests passed\n");
    return bad;
}
