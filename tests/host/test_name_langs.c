// SPDX-License-Identifier: Apache-2.0
#include <stdio.h>
#include <string.h>
#include "pet_text.h"
int main(void)
{
    char out[64];
    int bad = 0;
    pet_name_extract("안녕하세요, 저는 패트릭입니다", out, sizeof(out));
    bad |= strcmp(out, "패트릭") != 0;
    printf("ko: [%s]\n", out);
    pet_name_extract("こんにちは、私はパトリックです", out, sizeof(out));
    bad |= strcmp(out, "パトリック") != 0;
    printf("ja: [%s]\n", out);
    pet_name_extract("你好，我叫帕特里克", out, sizeof(out));
    bad |= strcmp(out, "帕特里克") != 0;
    printf("zh: [%s]\n", out);
    printf(bad ? "names: FAIL\n" : "names: all tests passed\n");
    return bad;
}
