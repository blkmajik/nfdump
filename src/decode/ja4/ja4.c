/*
 *  Copyright (c) 2024, Peter Haag
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright notice,
 *     this list of conditions and the following disclaimer in the documentation
 *     and/or other materials provided with the distribution.
 *   * Neither the name of SWITCH nor the names of its contributors may be
 *     used to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 *  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include "ja4.h"

#include <errno.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "digest/sha256.h"
#include "ssl/ssl.h"
#include "util.h"

#define MAX(X, Y) (((X) > (Y)) ? (X) : (Y))

static void sort(uint16Array_t array) {
    uint16_t *elements = array.array;
    uint32_t numElements = LenArray(array);

    for (int i = 0; i < numElements - 1; i++) {
        for (int j = 0; j < numElements - i - 1; j++) {
            if (elements[j] > elements[j + 1]) {
                uint16_t swap = elements[j];
                elements[j] = elements[j + 1];
                elements[j + 1] = swap;
            }
        }
    }
}  // End of sort

static int DecodeJA4(ja4_t *ja4, const uint8_t *data, size_t len, uint8_t proto) {
    // create ja4_a
    ja4->ja4.a[0] = proto == IPPROTO_TCP ? 't' : 'q';

    ja4->ja4.a[1] = ja4->ssl->tlsCharVersion[0];
    ja4->ja4.a[2] = ja4->ssl->tlsCharVersion[1];

    ja4->ja4.a[3] = ja4->ssl->sniName[0] ? 'd' : 'i';

    uint32_t num = LenArray(ja4->ssl->cipherSuites);
    if (num > 99) return 0;
    uint32_t ones = num % 10;
    uint32_t tens = num / 10;
    ja4->ja4.a[4] = tens + '0';
    ja4->ja4.a[5] = ones + '0';

    num = LenArray(ja4->ssl->extensions);
    if (num > 99) return 0;
    ones = num % 10;
    tens = num / 10;
    ja4->ja4.a[6] = tens + '0';
    ja4->ja4.a[7] = ones + '0';

    if (ja4->ssl->alpnName[0]) {
        // first and last char
        ja4->ja4.a[8] = ja4->ssl->alpnName[0];
        ja4->ja4.a[9] = ja4->ssl->alpnName[strlen(ja4->ssl->alpnName) - 1];
    } else {
        ja4->ja4.a[8] = '0';
        ja4->ja4.a[9] = '0';
    }

    // create ja4_b
    sort(ja4->ssl->cipherSuites);

    // generate string to sha256
    // create a string big enough for ciphersuites and extensions
    // uint16_t = max 5 digits + ',' = 6 digits per cipher + '\0'
    size_t maxStrLen = MAX(LenArray(ja4->ssl->cipherSuites), (LenArray(ja4->ssl->extensions) + LenArray(ja4->ssl->signatures))) * 6 + 1;
    char *hashString = (char *)malloc(maxStrLen);
    hashString[0] = '0';

    char hexChars[] = {'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
    uint32_t index = 0;
    for (int i = 0; i < LenArray(ja4->ssl->cipherSuites); i++) {
        uint16_t val = ArrayElement(ja4->ssl->cipherSuites, i);
        uint8_t n1 = val >> 12;
        uint8_t n2 = (val >> 8) & 0xF;
        uint8_t n3 = (val >> 4) & 0xF;
        uint8_t n4 = val & 0xF;
        hashString[index++] = hexChars[n1];
        hashString[index++] = hexChars[n2];
        hashString[index++] = hexChars[n3];
        hashString[index++] = hexChars[n4];
        hashString[index++] = ',';
    }
    // overwrite last ',' with end of string
    hashString[index - 1] = '\0';

    uint8_t sha256Digest[32];
    char sha256String[65];
    sha256((const unsigned char *)hashString, strlen(hashString), (unsigned char *)sha256Digest);

#ifdef DEVEL
    HexString(sha256Digest, 32, sha256String);
    printf("CipherString: %s\n", hashString);
    printf(" . Digest: %s\n", sha256String);
#else
    HexString(sha256Digest, 6, sha256String);
#endif

    memcpy((void *)ja4->ja4.b, (void *)sha256String, 12);
    ja4->ja4.b[12] = '\0';

    // create ja4_c
    sort(ja4->ssl->extensions);

    hashString[0] = '0';
    index = 0;
    for (int i = 0; i < LenArray(ja4->ssl->extensions); i++) {
        uint16_t val = ArrayElement(ja4->ssl->extensions, i);
        // skip extensions 0000 and 0010
        if (val == 0 || val == 0x10) continue;
        uint8_t n1 = val >> 12;
        uint8_t n2 = (val >> 8) & 0xF;
        uint8_t n3 = (val >> 4) & 0xF;
        uint8_t n4 = val & 0xF;
        hashString[index++] = hexChars[n1];
        hashString[index++] = hexChars[n2];
        hashString[index++] = hexChars[n3];
        hashString[index++] = hexChars[n4];
        hashString[index++] = ',';
    }
    hashString[index - 1] = '_';
    for (int i = 0; i < LenArray(ja4->ssl->signatures); i++) {
        uint16_t val = ArrayElement(ja4->ssl->signatures, i);
        uint8_t n1 = val >> 12;
        uint8_t n2 = (val >> 8) & 0xF;
        uint8_t n3 = (val >> 4) & 0xF;
        uint8_t n4 = val & 0xF;
        hashString[index++] = hexChars[n1];
        hashString[index++] = hexChars[n2];
        hashString[index++] = hexChars[n3];
        hashString[index++] = hexChars[n4];
        hashString[index++] = ',';
    }
    // overwrite last ',' with end of string
    hashString[index - 1] = '\0';

    sha256((const unsigned char *)hashString, strlen(hashString), (unsigned char *)sha256Digest);

#ifdef DEVEL
    HexString(sha256Digest, 32, sha256String);
    printf("ExtSigString: %s\n", hashString);
    printf(" . Digest: %s\n", sha256String);
#else
    HexString(sha256Digest, 6, sha256String);
#endif
    memcpy((void *)ja4->ja4.c, (void *)sha256String, 12);

    free(hashString);
    return 1;

}  // End of DecodeJA4

void ja4Print(ja4_t *ja4) {
    printf("SSL/TLS info:\n");
    sslPrint(ja4->ssl);

    printf("ja4:\n");
    printf("ja4  : %s_%s_%s\n", ja4->ja4.a, ja4->ja4.b, ja4->ja4.c);

}  // End of ja4Print

void ja4Free(ja4_t *ja4) {
    if (ja4) free(ja4);
}  // End of ja4Free

ja4_t *ja4Process(const uint8_t *data, size_t len, uint8_t proto) {
    ssl_t *ssl = sslProcess(data, len);
    if (!ssl) return NULL;

    ja4_t *ja4 = calloc(1, sizeof(ja4_t));
    if (!ja4) {
        LogError("calloc() error in %s line %d: %s\n", __FILE__, __LINE__, strerror(errno));
        return NULL;
    }
    ja4->ssl = ssl;

    if (DecodeJA4(ja4, data, len, proto) == 0) {
        free(ja4);
        return NULL;
    }

    return ja4;

}  // End of ja4Process

#ifdef MAIN

int main(int argc, char **argv) {
    const uint8_t tls12[] = {
        0x16, 0x03,                                      // ..X.....
        0x01, 0x02, 0x00, 0x01, 0x00, 0x01, 0xfc, 0x03,  // ........
        0x03, 0xec, 0xb2, 0x69, 0x1a, 0xdd, 0xb2, 0xbf,  // ...i....
        0x6c, 0x59, 0x9c, 0x7a, 0xaa, 0xe2, 0x3d, 0xe5,  // lY.z..=.
        0xf4, 0x25, 0x61, 0xcc, 0x04, 0xeb, 0x41, 0x02,  // .%a...A.
        0x9a, 0xcc, 0x6f, 0xc0, 0x50, 0xa1, 0x6a, 0xc1,  // ..o.P.j.
        0xd2, 0x20, 0x46, 0xf8, 0x61, 0x7b, 0x58, 0x0a,  // . F.a{X.
        0xc9, 0x35, 0x8e, 0x2a, 0xa4, 0x4e, 0x30, 0x6d,  // .5.*.N0m
        0x52, 0x46, 0x6b, 0xcc, 0x98, 0x9c, 0x87, 0xc8,  // RFk.....
        0xca, 0x64, 0x30, 0x9f, 0x5f, 0xaf, 0x50, 0xba,  // .d0._.P.
        0x7b, 0x4d, 0x00, 0x22, 0x13, 0x01, 0x13, 0x03,  // {M."....
        0x13, 0x02, 0xc0, 0x2b, 0xc0, 0x2f, 0xcc, 0xa9,  // ...+./..
        0xcc, 0xa8, 0xc0, 0x2c, 0xc0, 0x30, 0xc0, 0x0a,  // ...,.0..
        0xc0, 0x09, 0xc0, 0x13, 0xc0, 0x14, 0x00, 0x9c,  // ........
        0x00, 0x9d, 0x00, 0x2f, 0x00, 0x35, 0x01, 0x00,  // .../.5..
        0x01, 0x91, 0x00, 0x00, 0x00, 0x21, 0x00, 0x1f,  // .....!..
        0x00, 0x00, 0x1c, 0x63, 0x6f, 0x6e, 0x74, 0x69,  // ...conti
        0x6c, 0x65, 0x2e, 0x73, 0x65, 0x72, 0x76, 0x69,  // le.servi
        0x63, 0x65, 0x73, 0x2e, 0x6d, 0x6f, 0x7a, 0x69,  // ces.mozi
        0x6c, 0x6c, 0x61, 0x2e, 0x63, 0x6f, 0x6d, 0x00,  // lla.com.
        0x17, 0x00, 0x00, 0xff, 0x01, 0x00, 0x01, 0x00,  // ........
        0x00, 0x0a, 0x00, 0x0e, 0x00, 0x0c, 0x00, 0x1d,  // ........
        0x00, 0x17, 0x00, 0x18, 0x00, 0x19, 0x01, 0x00,  // ........
        0x01, 0x01, 0x00, 0x0b, 0x00, 0x02, 0x01, 0x00,  // ........
        0x00, 0x23, 0x00, 0x00, 0x00, 0x10, 0x00, 0x0e,  // .#......
        0x00, 0x0c, 0x02, 0x68, 0x32, 0x08, 0x68, 0x74,  // ...h2.ht
        0x74, 0x70, 0x2f, 0x31, 0x2e, 0x31, 0x00, 0x05,  // tp/1.1..
        0x00, 0x05, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,  // ........
        0x22, 0x00, 0x0a, 0x00, 0x08, 0x04, 0x03, 0x05,  // ".......
        0x03, 0x06, 0x03, 0x02, 0x03, 0x00, 0x33, 0x00,  // ......3.
        0x6b, 0x00, 0x69, 0x00, 0x1d, 0x00, 0x20, 0x89,  // k.i... .
        0x09, 0x85, 0x8f, 0xbe, 0xb6, 0xed, 0x2f, 0x12,  // ....../.
        0x48, 0xba, 0x5b, 0x9e, 0x29, 0x78, 0xbe, 0xad,  // H.[.)x..
        0x0e, 0x84, 0x01, 0x10, 0x19, 0x2c, 0x61, 0xda,  // .....,a.
        0xed, 0x00, 0x96, 0x79, 0x8b, 0x18, 0x44, 0x00,  // ...y..D.
        0x17, 0x00, 0x41, 0x04, 0x4d, 0x18, 0x3d, 0x91,  // ..A.M.=.
        0xf5, 0xee, 0xd3, 0x57, 0x91, 0xfa, 0x98, 0x24,  // ...W...$
        0x64, 0xe3, 0xb0, 0x21, 0x4a, 0xaa, 0x5f, 0x5d,  // d..!J._]
        0x1b, 0x78, 0x61, 0x6d, 0x9b, 0x9f, 0xbe, 0xbc,  // .xam....
        0x22, 0xd1, 0x1f, 0x53, 0x5b, 0x2f, 0x94, 0xc6,  // "..S[/..
        0x86, 0x14, 0x31, 0x36, 0xaa, 0x79, 0x5e, 0x6e,  // ..16.y^n
        0x5a, 0x87, 0x5d, 0x6c, 0x08, 0x06, 0x4a, 0xd5,  // Z.]l..J.
        0xb7, 0x6d, 0x44, 0xca, 0xad, 0x76, 0x6e, 0x24,  // .mD..vn$
        0x83, 0x01, 0x27, 0x48, 0x00, 0x2b, 0x00, 0x05,  // ..'H.+..
        0x04, 0x03, 0x04, 0x03, 0x03, 0x00, 0x0d, 0x00,  // ........
        0x18, 0x00, 0x16, 0x04, 0x03, 0x05, 0x03, 0x06,  // ........
        0x03, 0x08, 0x04, 0x08, 0x05, 0x08, 0x06, 0x04,  // ........
        0x01, 0x05, 0x01, 0x06, 0x01, 0x02, 0x03, 0x02,  // ........
        0x01, 0x00, 0x2d, 0x00, 0x02, 0x01, 0x01, 0x00,  // ..-.....
        0x1c, 0x00, 0x02, 0x40, 0x01, 0x00, 0x15, 0x00,  // ...@....
        0x7a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // z.......
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // ........
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // ........
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // ........
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // ........
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // ........
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // ........
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // ........
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // ........
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // ........
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // ........
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // ........
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // ........
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // ........
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // ........
        0x00, 0x00, 0x00                                 // ...
    };
    /*
    [JA4: t13d1715h2_5b57614c22b0_3d5424432f57]
    JA4_r:
    t13d1715h2_002f,0035,009c,009d,1301,1302,1303,c009,c00a,c013,c014,c02b,c02c,c02f,c030,cca8,cca9_0005,000a,000b,000d,0015,0017,001c,0022,0023,002b,002d,0033,ff01_0403,0503,0603,0804,0805,0806,0401,0501,0601,0203,0201]

    ja4 Fullstring:
    771,4865-4867-4866-49195-49199-52393-52392-49196-49200-49162-49161-49171-49172-156-157-47-53,0-23-65281-10-11-35-16-5-34-51-43-13-45-28-21,29-23-24-25-256-257,0]
    [ja4: 579ccef312d18482fc42e2b822ca2430]
    */
    // size_t len = sizeof(clientHello);
    size_t len = sizeof(tls12);

    ja4_t *ja4 = ja4Process(tls12, len, IPPROTO_TCP);
    if (ja4)
        ja4Print(ja4);
    else
        printf("Failed to parse ja4\n");

    return 0;
}

#endif