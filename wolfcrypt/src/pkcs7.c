/* pkcs7.c
 *
 * Copyright (C) 2006-2017 wolfSSL Inc.
 *
 * This file is part of wolfSSL.
 *
 * wolfSSL is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * wolfSSL is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335, USA
 */


#ifdef HAVE_CONFIG_H
    #include <config.h>
#endif

#include <wolfssl/wolfcrypt/settings.h>

#ifdef HAVE_PKCS7

#include <wolfssl/wolfcrypt/pkcs7.h>
#include <wolfssl/wolfcrypt/error-crypt.h>
#include <wolfssl/wolfcrypt/logging.h>
#include <wolfssl/wolfcrypt/hash.h>
#ifndef NO_RSA
    #include <wolfssl/wolfcrypt/rsa.h>
#endif
#ifdef HAVE_ECC
    #include <wolfssl/wolfcrypt/ecc.h>
#endif
#ifdef HAVE_LIBZ
    #include <wolfssl/wolfcrypt/compress.h>
#endif
#ifndef NO_PWDBASED
    #include <wolfssl/wolfcrypt/pwdbased.h>
#endif
#ifdef NO_INLINE
    #include <wolfssl/wolfcrypt/misc.h>
#else
    #define WOLFSSL_MISC_INCLUDED
    #include <wolfcrypt/src/misc.c>
#endif


/* direction for processing, encoding or decoding */
typedef enum {
    WC_PKCS7_ENCODE,
    WC_PKCS7_DECODE
} pkcs7Direction;

#ifndef NO_PKCS7_STREAM

#define MAX_PKCS7_STREAM_BUFFER 256
typedef struct PKCS7State {
    byte* tmpCert;
    byte* bufferPt;
    byte* key;
    byte* nonce;    /* stored nonce */
    byte* aad;      /* additional data for AEAD algos */
    byte* tag;      /* tag data for AEAD algos */
    byte* content;
    byte* buffer;   /* main internal read buffer */

    /* stack variables to store for when returning */
    word32 varOne;
    int    varTwo;
    int    varThree;

    word32 vers;
    word32 idx;      /* index read into current input buffer */
    word32 maxLen;   /* sanity cap on maximum amount of data to allow
                      * needed for GetSequence and other calls */
    word32 length;   /* amount of data stored */
    word32 bufferSz; /* size of internal buffer */
    word32 expected; /* next amount of data expected, if needed */
    word32 totalRd;  /* total amount of bytes read */
    word32 nonceSz;  /* size of nonce stored */
    word32 aadSz;    /* size of additional AEAD data */
    word32 tagSz;    /* size of tag for AEAD */
    word32 contentSz;
    byte tmpIv[MAX_CONTENT_IV_SIZE]; /* store IV if needed */
#ifdef WC_PKCS7_STREAM_DEBUG
    word32 peakUsed; /* most bytes used for struct at any one time */
    word32 peakRead; /* most bytes used by read buffer */
#endif
    byte   multi:1;  /* flag for if content is in multiple parts */
    byte   flagOne:1;
} PKCS7State;


enum PKCS7_MaxLen {
    PKCS7_DEFAULT_PEEK = 0,
    PKCS7_SEQ_PEEK
};

/* creates a PKCS7State structure and returns 0 on success */
static int wc_PKCS7_CreateStream(PKCS7* pkcs7)
{
    WOLFSSL_MSG("creating PKCS7 stream structure");
    pkcs7->stream = (PKCS7State*)XMALLOC(sizeof(PKCS7State), pkcs7->heap,
        DYNAMIC_TYPE_PKCS7);
    if (pkcs7->stream == NULL) {
        return MEMORY_E;
    }
    XMEMSET(pkcs7->stream, 0, sizeof(PKCS7State));
#ifdef WC_PKCS7_STREAM_DEBUG
    printf("\nCreating new PKCS#7 stream %p\n", pkcs7->stream);
#endif
    return 0;
}


static void wc_PKCS7_ResetStream(PKCS7* pkcs7)
{
    if (pkcs7 != NULL && pkcs7->stream != NULL) {
#ifdef WC_PKCS7_STREAM_DEBUG
        /* collect final data point in case more was read right before reset */
        if (pkcs7->stream->length > pkcs7->stream->peakRead) {
            pkcs7->stream->peakRead = pkcs7->stream->length;
        }
        if (pkcs7->stream->bufferSz + pkcs7->stream->aadSz +
                pkcs7->stream->nonceSz + pkcs7->stream->tagSz >
                pkcs7->stream->peakUsed) {
            pkcs7->stream->peakUsed = pkcs7->stream->bufferSz +
                pkcs7->stream->aadSz + pkcs7->stream->nonceSz +
                pkcs7->stream->tagSz;
        }

        /* print out debugging statistics */
        if (pkcs7->stream->peakUsed > 0 || pkcs7->stream->peakRead > 0) {
            printf("PKCS#7 STREAM:\n\tPeak heap used by struct = %d"
                                 "\n\tPeak read buffer bytes   = %d"
                                 "\n\tTotal bytes read         = %d"
                                 "\n",
                   pkcs7->stream->peakUsed, pkcs7->stream->peakRead,
                   pkcs7->stream->totalRd);
        }
        printf("PKCS#7 stream reset : Address [%p]\n", pkcs7->stream);
    #endif

        /* free any buffers that may be allocated */
        XFREE(pkcs7->stream->aad, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        XFREE(pkcs7->stream->tag, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        XFREE(pkcs7->stream->nonce, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        XFREE(pkcs7->stream->buffer, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        XFREE(pkcs7->stream->key, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        pkcs7->stream->aad    = NULL;
        pkcs7->stream->tag    = NULL;
        pkcs7->stream->nonce  = NULL;
        pkcs7->stream->buffer = NULL;
        pkcs7->stream->key    = NULL;

        /* reset values, note that content and tmpCert are saved */
        pkcs7->stream->maxLen   = 0;
        pkcs7->stream->length   = 0;
        pkcs7->stream->idx      = 0;
        pkcs7->stream->expected = 0;
        pkcs7->stream->totalRd  = 0;
        pkcs7->stream->bufferSz = 0;

        pkcs7->stream->multi    = 0;
        pkcs7->stream->flagOne  = 0;
        pkcs7->stream->varOne   = 0;
        pkcs7->stream->varTwo   = 0;
        pkcs7->stream->varThree = 0;
    }
}


static void wc_PKCS7_FreeStream(PKCS7* pkcs7)
{
    if (pkcs7 != NULL && pkcs7->stream != NULL) {
        wc_PKCS7_ResetStream(pkcs7);

        XFREE(pkcs7->stream->content, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        XFREE(pkcs7->stream->tmpCert, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        pkcs7->stream->content = NULL;
        pkcs7->stream->tmpCert = NULL;

        XFREE(pkcs7->stream, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        pkcs7->stream = NULL;
    }
}


/* used to increase the max size for internal buffer
 * returns 0 on success  */
static int wc_PKCS7_GrowStream(PKCS7* pkcs7, word32 newSz)
{
    byte* pt;

    pt = (byte*)XMALLOC(newSz, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
    if (pt == NULL) {
        return MEMORY_E;
    }
    XMEMCPY(pt, pkcs7->stream->buffer, pkcs7->stream->bufferSz);

#ifdef WC_PKCS7_STREAM_DEBUG
    printf("PKCS7 increasing internal stream buffer %d -> %d\n",
            pkcs7->stream->bufferSz, newSz);
#endif
    pkcs7->stream->bufferSz = newSz;
    XFREE(pkcs7->stream->buffer, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
    pkcs7->stream->buffer = pt;
    return 0;
}


/* pt gets set to the buffer that is holding data in the case that stream struct
 *    is used.
 *
 * Sets idx to be the current offset into "pt" buffer
 * returns 0 on success
 */
static int wc_PKCS7_AddDataToStream(PKCS7* pkcs7, byte* in, word32 inSz,
        word32 expected, byte** pt, word32* idx)
{
    word32 rdSz = pkcs7->stream->idx;

    /* If the input size minus current index into input buffer is greater than
     * the expected size then use the input buffer. If data is already stored
     * in stream buffer or if there is not enough input data available then use
     * the stream buffer. */
    if (inSz - rdSz >= expected && pkcs7->stream->length == 0) {
        /* storing input buffer is not needed */
        *pt  = in; /* reset in case previously used internal buffer */
        *idx = rdSz;
        return 0;
    }

    /* is there enough stored in buffer already? */
    if (pkcs7->stream->length >= expected) {
        *idx = 0; /* start reading from beginning of stream buffer */
        *pt  = pkcs7->stream->buffer;
        return 0;
    }

    /* check if all data has been read from input */
    if (rdSz >= inSz) {
        /* no more input to read, reset input index and request more data */
        pkcs7->stream->idx = 0;
        return WC_PKCS7_WANT_READ_E;
    }

    /* try to store input data into stream buffer */
    if (inSz - rdSz > 0 && pkcs7->stream->length < expected) {
        int len = min(inSz - rdSz, expected - pkcs7->stream->length);

        /* check if internal buffer size needs to be increased */
        if (len + pkcs7->stream->length > pkcs7->stream->bufferSz) {
            int ret = wc_PKCS7_GrowStream(pkcs7, expected);
            if (ret < 0) {
                return ret;
            }
        }
        XMEMCPY(pkcs7->stream->buffer + pkcs7->stream->length, in + rdSz, len);
        pkcs7->stream->length  += len;
        pkcs7->stream->idx     += len;
        pkcs7->stream->totalRd += len;
    }

#ifdef WC_PKCS7_STREAM_DEBUG
    /* collects memory usage for debugging */
    if (pkcs7->stream->length > pkcs7->stream->peakRead) {
        pkcs7->stream->peakRead = pkcs7->stream->length;
    }
    if (pkcs7->stream->bufferSz + pkcs7->stream->aadSz + pkcs7->stream->nonceSz +
        pkcs7->stream->tagSz > pkcs7->stream->peakUsed) {
        pkcs7->stream->peakUsed = pkcs7->stream->bufferSz +
           pkcs7->stream->aadSz + pkcs7->stream->nonceSz + pkcs7->stream->tagSz;
    }
#endif

    /* if not enough data was read in then request more */
    if (pkcs7->stream->length < expected) {
        pkcs7->stream->idx = 0;
        return WC_PKCS7_WANT_READ_E;
    }

    /* adjust pointer to read from stored buffer */
    *idx = 0;
    *pt  = pkcs7->stream->buffer;
    return 0;
}


/* Does two things
 *  1) Tries to get the length from current buffer and set it as max length
 *  2) Retrieves the set max length
 *
 * if no flag value is set then the stored max length is returned.
 * returns length found on success and defSz if no stored data is found
 */
static word32 wc_PKCS7_GetMaxStream(PKCS7* pkcs7, byte flag, byte* in,
        word32 defSz)
{
    /* check there is a buffer to read from */
    if (pkcs7) {
        int     length = 0, ret;
        word32  idx = 0;
        byte*   pt;

        if (flag != PKCS7_DEFAULT_PEEK) {
            if (pkcs7->stream->length > 0) {
                length = pkcs7->stream->length;
                pt     = pkcs7->stream->buffer;
            }
            else {
                length = defSz;
                pt     = in;
            }

            if (length < MAX_SEQ_SZ) {
                WOLFSSL_MSG("PKCS7 Error not enough data for SEQ peek\n");
                return 0;
            }
            if (flag == PKCS7_SEQ_PEEK) {
                if ((ret = GetSequence(pt, &idx, &length, (word32)-1)) < 0) {
                    return ret;
                }
                pkcs7->stream->maxLen = length + idx;
            }
        }
        return pkcs7->stream->maxLen;
    }

    if (pkcs7->stream->maxLen == 0) {
        pkcs7->stream->maxLen = defSz;
    }
    return defSz;
}


/* setter function for stored variables */
static void wc_PKCS7_StreamStoreVar(PKCS7* pkcs7, word32 var1, int var2,
        int var3)
{
    if (pkcs7 != NULL && pkcs7->stream != NULL) {
        pkcs7->stream->varOne   = var1;
        pkcs7->stream->varTwo   = var2;
        pkcs7->stream->varThree = var3;
    }
}

/* getter function for stored variables */
static void wc_PKCS7_StreamGetVar(PKCS7* pkcs7, word32* var1, int* var2,
        int* var3)
{
    if (pkcs7 != NULL && pkcs7->stream != NULL) {
        if (var1 != NULL) *var1 = pkcs7->stream->varOne;
        if (var2 != NULL) *var2 = pkcs7->stream->varTwo;
        if (var3 != NULL) *var3 = pkcs7->stream->varThree;
    }
}


/* common update of index and total read after section complete
 * returns 0 on success */
static int wc_PKCS7_StreamEndCase(PKCS7* pkcs7, word32* tmpIdx, word32* idx)
{
    int ret = 0;

    if (pkcs7->stream->length > 0) {
        if (pkcs7->stream->length < *idx) {
            WOLFSSL_MSG("PKCS7 read too much data from internal buffer");
            ret = BUFFER_E;
        }
        else {
            XMEMMOVE(pkcs7->stream->buffer, pkcs7->stream->buffer + *idx,
                 pkcs7->stream->length - *idx);
            pkcs7->stream->length -= *idx;
        }
    }
    else {
        pkcs7->stream->totalRd += *idx - *tmpIdx;
        pkcs7->stream->idx = *idx; /* adjust index into input buffer */
        *tmpIdx = *idx;
    }

    return ret;
}
#endif /* NO_PKCS7_STREAM */

#ifdef WC_PKCS7_STREAM_DEBUG
/* used to print out human readable state for debugging */
static const char* wc_PKCS7_GetStateName(int in)
{
    switch (in) {
        case WC_PKCS7_START: return "WC_PKCS7_START";

        case WC_PKCS7_STAGE2: return "WC_PKCS7_STAGE2";
        case WC_PKCS7_STAGE3: return "WC_PKCS7_STAGE3";
        case WC_PKCS7_STAGE4: return "WC_PKCS7_STAGE4";
        case WC_PKCS7_STAGE5: return "WC_PKCS7_STAGE5";
        case WC_PKCS7_STAGE6: return "WC_PKCS7_STAGE6";

        /* parse info set */
        case WC_PKCS7_INFOSET_START:  return "WC_PKCS7_INFOSET_START";
        case WC_PKCS7_INFOSET_BER:    return "WC_PKCS7_INFOSET_BER";
        case WC_PKCS7_INFOSET_STAGE1: return "WC_PKCS7_INFOSET_STAGE1";
        case WC_PKCS7_INFOSET_STAGE2: return "WC_PKCS7_INFOSET_STAGE2";
        case WC_PKCS7_INFOSET_END:    return "WC_PKCS7_INFOSET_END";

        /* decode enveloped data */
        case WC_PKCS7_ENV_2: return "WC_PKCS7_ENV_2";
        case WC_PKCS7_ENV_3: return "WC_PKCS7_ENV_3";
        case WC_PKCS7_ENV_4: return "WC_PKCS7_ENV_4";
        case WC_PKCS7_ENV_5: return "WC_PKCS7_ENV_5";

        /* decode auth enveloped */
        case WC_PKCS7_AUTHENV_2: return "WC_PKCS7_AUTHENV_2";
        case WC_PKCS7_AUTHENV_3: return "WC_PKCS7_AUTHENV_3";
        case WC_PKCS7_AUTHENV_4: return "WC_PKCS7_AUTHENV_4";
        case WC_PKCS7_AUTHENV_5: return "WC_PKCS7_AUTHENV_5";
        case WC_PKCS7_AUTHENV_6: return "WC_PKCS7_AUTHENV_6";
        case WC_PKCS7_AUTHENV_ATRB: return "WC_PKCS7_AUTHENV_ATRB";
        case WC_PKCS7_AUTHENV_ATRBEND: return "WC_PKCS7_AUTHENV_ATRBEND";
        case WC_PKCS7_AUTHENV_7: return "WC_PKCS7_AUTHENV_7";

        /* decryption state types */
        case WC_PKCS7_DECRYPT_KTRI:   return "WC_PKCS7_DECRYPT_KTRI";
        case WC_PKCS7_DECRYPT_KTRI_2: return "WC_PKCS7_DECRYPT_KTRI_2";
        case WC_PKCS7_DECRYPT_KTRI_3: return "WC_PKCS7_DECRYPT_KTRI_3";

        case WC_PKCS7_DECRYPT_KARI:  return "WC_PKCS7_DECRYPT_KARI";
        case WC_PKCS7_DECRYPT_KEKRI: return "WC_PKCS7_DECRYPT_KEKRI";
        case WC_PKCS7_DECRYPT_PWRI:  return "WC_PKCS7_DECRYPT_PWRI";
        case WC_PKCS7_DECRYPT_ORI:   return "WC_PKCS7_DECRYPT_ORI";
        case WC_PKCS7_DECRYPT_DONE:  return "WC_PKCS7_DECRYPT_DONE";

        case WC_PKCS7_VERIFY_STAGE2: return "WC_PKCS7_VERIFY_STAGE2";
        case WC_PKCS7_VERIFY_STAGE3: return "WC_PKCS7_VERIFY_STAGE3";
        case WC_PKCS7_VERIFY_STAGE4: return "WC_PKCS7_VERIFY_STAGE4";
        case WC_PKCS7_VERIFY_STAGE5: return "WC_PKCS7_VERIFY_STAGE5";
        case WC_PKCS7_VERIFY_STAGE6: return "WC_PKCS7_VERIFY_STAGE6";

        default:
            return "Unknown state";
    }
}
#endif

/* Used to change the PKCS7 state. Having state change as a function allows
 * for easier debugging */
static void wc_PKCS7_ChangeState(PKCS7* pkcs7, int newState)
{
#ifdef WC_PKCS7_STREAM_DEBUG
    printf("\tChanging from state [%02d] %s to [%02d] %s\n",
            pkcs7->state, wc_PKCS7_GetStateName(pkcs7->state),
            newState, wc_PKCS7_GetStateName(newState));
#endif
    pkcs7->state = newState;
}

#define MAX_PKCS7_DIGEST_SZ (MAX_SEQ_SZ + MAX_ALGO_SZ + \
                             MAX_OCTET_STR_SZ + WC_MAX_DIGEST_SIZE)


/* placed ASN.1 contentType OID into *output, return idx on success,
 * 0 upon failure */
static int wc_SetContentType(int pkcs7TypeOID, byte* output, word32 outputSz)
{
    /* PKCS#7 content types, RFC 2315, section 14 */
    const byte pkcs7[]              = { 0x2A, 0x86, 0x48, 0x86, 0xF7,
                                               0x0D, 0x01, 0x07 };
    const byte data[]               = { 0x2A, 0x86, 0x48, 0x86, 0xF7,
                                               0x0D, 0x01, 0x07, 0x01 };
    const byte signedData[]         = { 0x2A, 0x86, 0x48, 0x86, 0xF7,
                                               0x0D, 0x01, 0x07, 0x02};
    const byte envelopedData[]      = { 0x2A, 0x86, 0x48, 0x86, 0xF7,
                                               0x0D, 0x01, 0x07, 0x03 };
    const byte authEnvelopedData[]  = { 0x2A, 0x86, 0x48, 0x86, 0xF7,
                                        0x0D, 0x01, 0x09, 0x10, 0x01, 0x17};
    const byte signedAndEnveloped[] = { 0x2A, 0x86, 0x48, 0x86, 0xF7,
                                               0x0D, 0x01, 0x07, 0x04 };
    const byte digestedData[]       = { 0x2A, 0x86, 0x48, 0x86, 0xF7,
                                               0x0D, 0x01, 0x07, 0x05 };
#ifndef NO_PKCS7_ENCRYPTED_DATA
    const byte encryptedData[]      = { 0x2A, 0x86, 0x48, 0x86, 0xF7,
                                               0x0D, 0x01, 0x07, 0x06 };
#endif
    /* FirmwarePkgData (1.2.840.113549.1.9.16.1.16), RFC 4108 */
    const byte firmwarePkgData[]    = { 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D,
                                        0x01, 0x09, 0x10, 0x01, 0x10 };
#if defined(HAVE_LIBZ) && !defined(NO_PKCS7_COMPRESSED_DATA)
    /* id-ct-compressedData (1.2.840.113549.1.9.16.1.9), RFC 3274 */
    const byte compressedData[]     = { 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D,
                                        0x01, 0x09, 0x10, 0x01, 0x09 };
#endif

#ifndef NO_PWDBASED
    const byte pwriKek[]            = { 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D,
                                        0x01, 0x09, 0x10, 0x03, 0x09 };
    const byte pbkdf2[]             = { 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D,
                                        0x01, 0x05, 0x0C };
#endif

    int idSz, idx = 0;
    word32 typeSz = 0;
    const byte* typeName = 0;
    byte ID_Length[MAX_LENGTH_SZ];

    switch (pkcs7TypeOID) {
        case PKCS7_MSG:
            typeSz = sizeof(pkcs7);
            typeName = pkcs7;
            break;

        case DATA:
            typeSz = sizeof(data);
            typeName = data;
            break;

        case SIGNED_DATA:
            typeSz = sizeof(signedData);
            typeName = signedData;
            break;

        case ENVELOPED_DATA:
            typeSz = sizeof(envelopedData);
            typeName = envelopedData;
            break;

        case AUTH_ENVELOPED_DATA:
            typeSz = sizeof(authEnvelopedData);
            typeName = authEnvelopedData;
            break;

        case SIGNED_AND_ENVELOPED_DATA:
            typeSz = sizeof(signedAndEnveloped);
            typeName = signedAndEnveloped;
            break;

        case DIGESTED_DATA:
            typeSz = sizeof(digestedData);
            typeName = digestedData;
            break;

#ifndef NO_PKCS7_ENCRYPTED_DATA
        case ENCRYPTED_DATA:
            typeSz = sizeof(encryptedData);
            typeName = encryptedData;
            break;
#endif
#if defined(HAVE_LIBZ) && !defined(NO_PKCS7_COMPRESSED_DATA)
        case COMPRESSED_DATA:
            typeSz = sizeof(compressedData);
            typeName = compressedData;
            break;
#endif
        case FIRMWARE_PKG_DATA:
            typeSz = sizeof(firmwarePkgData);
            typeName = firmwarePkgData;
            break;

#ifndef NO_PWDBASED
        case PWRI_KEK_WRAP:
            typeSz = sizeof(pwriKek);
            typeName = pwriKek;
            break;

        case PBKDF2_OID:
            typeSz = sizeof(pbkdf2);
            typeName = pbkdf2;
            break;
#endif

        default:
            WOLFSSL_MSG("Unknown PKCS#7 Type");
            return 0;
    };

    if (outputSz < (MAX_LENGTH_SZ + 1 + typeSz)) {
        WOLFSSL_MSG("CMS content type buffer too small");
        return BAD_FUNC_ARG;
    }

    idSz  = SetLength(typeSz, ID_Length);
    output[idx++] = ASN_OBJECT_ID;
    XMEMCPY(output + idx, ID_Length, idSz);
    idx += idSz;
    XMEMCPY(output + idx, typeName, typeSz);
    idx += typeSz;

    return idx;
}


/* get ASN.1 contentType OID sum, return 0 on success, <0 on failure */
static int wc_GetContentType(const byte* input, word32* inOutIdx, word32* oid,
                             word32 maxIdx)
{
    WOLFSSL_ENTER("wc_GetContentType");
    if (GetObjectId(input, inOutIdx, oid, oidIgnoreType, maxIdx) < 0)
        return ASN_PARSE_E;

    return 0;
}


/* return block size for algorithm represented by oid, or <0 on error */
static int wc_PKCS7_GetOIDBlockSize(int oid)
{
    int blockSz;

    switch (oid) {
#ifndef NO_AES
    #ifdef WOLFSSL_AES_128
        case AES128CBCb:
        case AES128GCMb:
        case AES128CCMb:
    #endif
    #ifdef WOLFSSL_AES_192
        case AES192CBCb:
        case AES192GCMb:
        case AES192CCMb:
    #endif
    #ifdef WOLFSSL_AES_256
        case AES256CBCb:
        case AES256GCMb:
        case AES256CCMb:
    #endif
            blockSz = AES_BLOCK_SIZE;
            break;
#endif
#ifndef NO_DES3
        case DESb:
        case DES3b:
            blockSz = DES_BLOCK_SIZE;
            break;
#endif
        default:
            WOLFSSL_MSG("Unsupported content cipher type");
            return ALGO_ID_E;
    };

    return blockSz;
}


/* get key size for algorithm represented by oid, or <0 on error */
static int wc_PKCS7_GetOIDKeySize(int oid)
{
    int blockKeySz;

    switch (oid) {
#ifndef NO_AES
    #ifdef WOLFSSL_AES_128
        case AES128CBCb:
        case AES128GCMb:
        case AES128CCMb:
        case AES128_WRAP:
            blockKeySz = 16;
            break;
    #endif
    #ifdef WOLFSSL_AES_192
        case AES192CBCb:
        case AES192GCMb:
        case AES192CCMb:
        case AES192_WRAP:
            blockKeySz = 24;
            break;
    #endif
    #ifdef WOLFSSL_AES_256
        case AES256CBCb:
        case AES256GCMb:
        case AES256CCMb:
        case AES256_WRAP:
            blockKeySz = 32;
            break;
    #endif
#endif
#ifndef NO_DES3
        case DESb:
            blockKeySz = DES_KEYLEN;
            break;

        case DES3b:
            blockKeySz = DES3_KEYLEN;
            break;
#endif
        default:
            WOLFSSL_MSG("Unsupported content cipher type");
            return ALGO_ID_E;
    };

    return blockKeySz;
}


PKCS7* wc_PKCS7_New(void* heap, int devId)
{
    PKCS7* pkcs7 = (PKCS7*)XMALLOC(sizeof(PKCS7), heap, DYNAMIC_TYPE_PKCS7);
    if (pkcs7) {
        XMEMSET(pkcs7, 0, sizeof(PKCS7));
        if (wc_PKCS7_Init(pkcs7, heap, devId) == 0) {
            pkcs7->isDynamic = 1;
        }
        else {
            XFREE(pkcs7, heap, DYNAMIC_TYPE_PKCS7);
            pkcs7 = NULL;
        }
    }
    return pkcs7;
}

/* This is to initialize a PKCS7 structure. It sets all values to 0 and can be
 * used to set the heap hint.
 *
 * pkcs7 PKCS7 structure to initialize
 * heap  memory heap hint for PKCS7 structure to use
 * devId currently not used but a place holder for async operations
 *
 * returns 0 on success or a negative value for failure
 */
int wc_PKCS7_Init(PKCS7* pkcs7, void* heap, int devId)
{
    word16 isDynamic;

    WOLFSSL_ENTER("wc_PKCS7_Init");

    if (pkcs7 == NULL) {
        return BAD_FUNC_ARG;
    }

    isDynamic = pkcs7->isDynamic;
    XMEMSET(pkcs7, 0, sizeof(PKCS7));
    pkcs7->isDynamic = isDynamic;
#ifdef WOLFSSL_HEAP_TEST
    pkcs7->heap = (void*)WOLFSSL_HEAP_TEST;
#else
    pkcs7->heap = heap;
#endif
    pkcs7->devId = devId;

    return 0;
}


/* Certificate structure holding der pointer, size, and pointer to next
 * Pkcs7Cert struct. Used when creating SignedData types with multiple
 * certificates. */
typedef struct Pkcs7Cert {
    byte*  der;
    word32 derSz;
    Pkcs7Cert* next;
} Pkcs7Cert;


/* Linked list of ASN.1 encoded RecipientInfos */
typedef struct Pkcs7EncodedRecip {
    byte recip[MAX_RECIP_SZ];
    word32 recipSz;
    int recipType;
    int recipVersion;
    Pkcs7EncodedRecip* next;
} Pkcs7EncodedRecip;


/* free all members of Pkcs7Cert linked list */
static void wc_PKCS7_FreeCertSet(PKCS7* pkcs7)
{
    Pkcs7Cert* curr = NULL;
    Pkcs7Cert* next = NULL;

    if (pkcs7 == NULL)
        return;

    curr = pkcs7->certList;
    pkcs7->certList = NULL;

    while (curr != NULL) {
        next = curr->next;
        curr->next = NULL;
        XFREE(curr, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        curr = next;
    }

    return;
}


/* Get total size of all recipients in recipient list.
 *
 * Returns total size of recipients, or negative upon error */
static int wc_PKCS7_GetRecipientListSize(PKCS7* pkcs7)
{
    int totalSz = 0;
    Pkcs7EncodedRecip* tmp = NULL;

    if (pkcs7 == NULL)
        return BAD_FUNC_ARG;

    tmp = pkcs7->recipList;

    while (tmp != NULL) {
        totalSz += tmp->recipSz;
        tmp = tmp->next;
    }

    return totalSz;
}


/* free all members of Pkcs7EncodedRecip linked list */
static void wc_PKCS7_FreeEncodedRecipientSet(PKCS7* pkcs7)
{
    Pkcs7EncodedRecip* curr = NULL;
    Pkcs7EncodedRecip* next = NULL;

    if (pkcs7 == NULL)
        return;

    curr = pkcs7->recipList;
    pkcs7->recipList = NULL;

    while (curr != NULL) {
        next = curr->next;
        curr->next = NULL;
        XFREE(curr, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        curr = next;
    }

    return;
}


/* search through RecipientInfo list for specific type.
 * return 1 if ANY recipient of type specified is present, otherwise
 * return 0 */
static int wc_PKCS7_RecipientListIncludesType(PKCS7* pkcs7, int type)
{
    Pkcs7EncodedRecip* tmp = NULL;

    if (pkcs7 == NULL)
        return BAD_FUNC_ARG;

    tmp = pkcs7->recipList;

    while (tmp != NULL) {
        if (tmp->recipType == type)
            return 1;

        tmp = tmp->next;
    }

    return 0;
}


/* searches through RecipientInfo list, returns 1 if all structure
 * versions are set to 0, otherwise returns 0 */
static int wc_PKCS7_RecipientListVersionsAllZero(PKCS7* pkcs7)
{
    Pkcs7EncodedRecip* tmp = NULL;

    if (pkcs7 == NULL)
        return BAD_FUNC_ARG;

    tmp = pkcs7->recipList;

    while (tmp != NULL) {
        if (tmp->recipVersion != 0)
            return 0;

        tmp = tmp->next;
    }

    return 1;
}


/* Init PKCS7 struct with recipient cert, decode into DecodedCert
 * NOTE: keeps previously set pkcs7 heap hint, devId and isDynamic */
int wc_PKCS7_InitWithCert(PKCS7* pkcs7, byte* derCert, word32 derCertSz)
{
    int ret = 0;
    void* heap;
    int devId;
    Pkcs7Cert* cert;
    Pkcs7Cert* lastCert;

    if (pkcs7 == NULL || (derCert == NULL && derCertSz != 0)) {
        return BAD_FUNC_ARG;
    }

    heap = pkcs7->heap;
    devId = pkcs7->devId;
    ret = wc_PKCS7_Init(pkcs7, heap, devId);
    if (ret != 0)
        return ret;

    if (derCert != NULL && derCertSz > 0) {
#ifdef WOLFSSL_SMALL_STACK
        DecodedCert* dCert;

        dCert = (DecodedCert*)XMALLOC(sizeof(DecodedCert), pkcs7->heap,
                                                       DYNAMIC_TYPE_DCERT);
        if (dCert == NULL)
            return MEMORY_E;
#else
        DecodedCert dCert[1];
#endif

        pkcs7->singleCert = derCert;
        pkcs7->singleCertSz = derCertSz;

        /* create new Pkcs7Cert for recipient, freed during cleanup */
        cert = (Pkcs7Cert*)XMALLOC(sizeof(Pkcs7Cert), pkcs7->heap,
                                   DYNAMIC_TYPE_PKCS7);
        XMEMSET(cert, 0, sizeof(Pkcs7Cert));
        cert->der = derCert;
        cert->derSz = derCertSz;
        cert->next = NULL;

        /* free existing cert list if existing */
        wc_PKCS7_FreeCertSet(pkcs7);

        /* add cert to list */
        if (pkcs7->certList == NULL) {
            pkcs7->certList = cert;
        } else {
           lastCert = pkcs7->certList;
           while (lastCert->next != NULL) {
               lastCert = lastCert->next;
           }
           lastCert->next = cert;
        }

        InitDecodedCert(dCert, derCert, derCertSz, pkcs7->heap);
        ret = ParseCert(dCert, CA_TYPE, NO_VERIFY, 0);
        if (ret < 0) {
            FreeDecodedCert(dCert);
#ifdef WOLFSSL_SMALL_STACK
            XFREE(dCert, pkcs7->heap, DYNAMIC_TYPE_DCERT);
#endif
            return ret;
        }

        XMEMCPY(pkcs7->publicKey, dCert->publicKey, dCert->pubKeySize);
        pkcs7->publicKeySz = dCert->pubKeySize;
        pkcs7->publicKeyOID = dCert->keyOID;
        XMEMCPY(pkcs7->issuerHash, dCert->issuerHash, KEYID_SIZE);
        pkcs7->issuer = dCert->issuerRaw;
        pkcs7->issuerSz = dCert->issuerRawLen;
        XMEMCPY(pkcs7->issuerSn, dCert->serial, dCert->serialSz);
        pkcs7->issuerSnSz = dCert->serialSz;
        XMEMCPY(pkcs7->issuerSubjKeyId, dCert->extSubjKeyId, KEYID_SIZE);

        /* default to IssuerAndSerialNumber for SignerIdentifier */
        pkcs7->sidType = CMS_ISSUER_AND_SERIAL_NUMBER;

        /* free existing recipient list if existing */
        wc_PKCS7_FreeEncodedRecipientSet(pkcs7);

        FreeDecodedCert(dCert);

#ifdef WOLFSSL_SMALL_STACK
        XFREE(dCert, pkcs7->heap, DYNAMIC_TYPE_DCERT);
#endif
    }

    return ret;
}


/* Adds one DER-formatted certificate to the internal PKCS7/CMS certificate
 * list, to be added as part of the certificates CertificateSet. Currently
 * used in SignedData content type.
 *
 * Must be called after wc_PKCS7_Init() or wc_PKCS7_InitWithCert().
 *
 * Does not represent the recipient/signer certificate, only certificates that
 * are part of the certificate chain used to build and verify signer
 * certificates.
 *
 * This API does not currently validate certificates.
 *
 * Returns 0 on success, negative upon error */
int wc_PKCS7_AddCertificate(PKCS7* pkcs7, byte* derCert, word32 derCertSz)
{
    Pkcs7Cert* cert;

    if (pkcs7 == NULL || derCert == NULL || derCertSz == 0)
        return BAD_FUNC_ARG;

    cert = (Pkcs7Cert*)XMALLOC(sizeof(Pkcs7Cert), pkcs7->heap,
                               DYNAMIC_TYPE_PKCS7);
    if (cert == NULL)
        return MEMORY_E;

    cert->der = derCert;
    cert->derSz = derCertSz;

    if (pkcs7->certList == NULL) {
        pkcs7->certList = cert;
    } else {
        cert->next = pkcs7->certList;
        pkcs7->certList = cert;
    }

    return 0;
}


/* free linked list of PKCS7DecodedAttrib structs */
static void wc_PKCS7_FreeDecodedAttrib(PKCS7DecodedAttrib* attrib, void* heap)
{
    PKCS7DecodedAttrib* current;

    if (attrib == NULL) {
        return;
    }

    current = attrib;
    while (current != NULL) {
        PKCS7DecodedAttrib* next = current->next;
        if (current->oid != NULL)  {
            XFREE(current->oid, heap, DYNAMIC_TYPE_PKCS7);
        }
        if (current->value != NULL) {
            XFREE(current->value, heap, DYNAMIC_TYPE_PKCS7);
        }
        XFREE(current, heap, DYNAMIC_TYPE_PKCS7);
        current = next;
    }

    (void)heap;
}


/* releases any memory allocated by a PKCS7 initializer */
void wc_PKCS7_Free(PKCS7* pkcs7)
{
    if (pkcs7 == NULL)
        return;

#ifndef NO_PKCS7_STREAM
    wc_PKCS7_FreeStream(pkcs7);
#endif

    wc_PKCS7_FreeDecodedAttrib(pkcs7->decodedAttrib, pkcs7->heap);
    wc_PKCS7_FreeCertSet(pkcs7);

#ifdef ASN_BER_TO_DER
    if (pkcs7->der != NULL)
        XFREE(pkcs7->der, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
#endif
    if (pkcs7->contentDynamic != NULL)
        XFREE(pkcs7->contentDynamic, pkcs7->heap, DYNAMIC_TYPE_PKCS7);

    if (pkcs7->cek != NULL) {
        ForceZero(pkcs7->cek, pkcs7->cekSz);
        XFREE(pkcs7->cek, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
    }

    pkcs7->contentTypeSz = 0;

    if (pkcs7->isDynamic) {
        pkcs7->isDynamic = 0;
        XFREE(pkcs7, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
    }
}


/* helper function for parsing through attributes and finding a specific one.
 * returns PKCS7DecodedAttrib pointer on success */
static PKCS7DecodedAttrib* findAttrib(PKCS7* pkcs7, const byte* oid, word32 oidSz)
{
    PKCS7DecodedAttrib* list;

    if (pkcs7 == NULL || oid == NULL) {
        return NULL;
    }

    /* search attributes for pkiStatus */
    list = pkcs7->decodedAttrib;
    while (list != NULL) {
        word32 sz  = oidSz;
        word32 idx = 0;
        int    length = 0;

        if (list->oid[idx++] != ASN_OBJECT_ID) {
            WOLFSSL_MSG("Bad attribute ASN1 syntax");
            return NULL;
        }

        if (GetLength(list->oid, &idx, &length, list->oidSz) < 0) {
            WOLFSSL_MSG("Bad attribute length");
            return NULL;
        }

        sz = (sz < (word32)length)? sz : (word32)length;
        if (XMEMCMP(oid, list->oid + idx, sz) == 0) {
            return list;
        }
        list = list->next;
    }
    return NULL;
}


/* Searches through decoded attributes and returns the value for the first one
 * matching the oid passed in. Note that this value includes the leading ASN1
 * syntax. So for a printable string of "3" this would be something like
 *
 * 0x13, 0x01, 0x33
 *  ID   SIZE  "3"
 *
 * pkcs7  structure to get value from
 * oid    OID value to search for with attributes
 * oidSz  size of oid buffer
 * out    buffer to hold result
 * outSz  size of out buffer (if out is NULL this is set to needed size and
          LENGTH_ONLY_E is returned)
 *
 * returns size of value on success
 */
int wc_PKCS7_GetAttributeValue(PKCS7* pkcs7, const byte* oid, word32 oidSz,
        byte* out, word32* outSz)
{
    PKCS7DecodedAttrib* attrib;

    if (pkcs7 == NULL || oid == NULL || outSz == NULL) {
        return BAD_FUNC_ARG;
    }

    attrib = findAttrib(pkcs7, oid, oidSz);
    if (attrib == NULL) {
        return ASN_PARSE_E;
    }

    if (out == NULL) {
        *outSz = attrib->valueSz;
        return LENGTH_ONLY_E;
    }

    if (*outSz < attrib->valueSz) {
        return BUFFER_E;
    }

    XMEMCPY(out, attrib->value, attrib->valueSz);
    return attrib->valueSz;
}


/* build PKCS#7 data content type */
int wc_PKCS7_EncodeData(PKCS7* pkcs7, byte* output, word32 outputSz)
{
    static const byte oid[] =
        { ASN_OBJECT_ID, 0x09, 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01,
                         0x07, 0x01 };
    byte seq[MAX_SEQ_SZ];
    byte octetStr[MAX_OCTET_STR_SZ];
    word32 seqSz;
    word32 octetStrSz;
    word32 oidSz = (word32)sizeof(oid);
    int idx = 0;

    if (pkcs7 == NULL || output == NULL) {
        return BAD_FUNC_ARG;
    }

    octetStrSz = SetOctetString(pkcs7->contentSz, octetStr);
    seqSz = SetSequence(pkcs7->contentSz + octetStrSz + oidSz, seq);

    if (outputSz < pkcs7->contentSz + octetStrSz + oidSz + seqSz)
        return BUFFER_E;

    XMEMCPY(output, seq, seqSz);
    idx += seqSz;
    XMEMCPY(output + idx, oid, oidSz);
    idx += oidSz;
    XMEMCPY(output + idx, octetStr, octetStrSz);
    idx += octetStrSz;
    XMEMCPY(output + idx, pkcs7->content, pkcs7->contentSz);
    idx += pkcs7->contentSz;

    return idx;
}


typedef struct EncodedAttrib {
    byte valueSeq[MAX_SEQ_SZ];
        const byte* oid;
        byte valueSet[MAX_SET_SZ];
        const byte* value;
    word32 valueSeqSz, oidSz, idSz, valueSetSz, valueSz, totalSz;
} EncodedAttrib;


typedef struct ESD {
    wc_HashAlg  hash;
    enum wc_HashType hashType;
    byte contentDigest[WC_MAX_DIGEST_SIZE + 2]; /* content only + ASN.1 heading */
    byte contentAttribsDigest[WC_MAX_DIGEST_SIZE];
    byte encContentDigest[MAX_ENCRYPTED_KEY_SZ];

    byte outerSeq[MAX_SEQ_SZ];
        byte outerContent[MAX_EXP_SZ];
            byte innerSeq[MAX_SEQ_SZ];
                byte version[MAX_VERSION_SZ];
                byte digAlgoIdSet[MAX_SET_SZ];
                    byte singleDigAlgoId[MAX_ALGO_SZ];

                byte contentInfoSeq[MAX_SEQ_SZ];
                    byte innerContSeq[MAX_EXP_SZ];
                        byte innerOctets[MAX_OCTET_STR_SZ];

                byte certsSet[MAX_SET_SZ];

                byte signerInfoSet[MAX_SET_SZ];
                    byte signerInfoSeq[MAX_SEQ_SZ];
                        byte signerVersion[MAX_VERSION_SZ];
                        /* issuerAndSerialNumber ...*/
                        byte issuerSnSeq[MAX_SEQ_SZ];
                            byte issuerName[MAX_SEQ_SZ];
                            byte issuerSn[MAX_SN_SZ];
                        /* OR subjectKeyIdentifier */
                        byte issuerSKIDSeq[MAX_SEQ_SZ];
                            byte issuerSKID[MAX_OCTET_STR_SZ];
                        byte signerDigAlgoId[MAX_ALGO_SZ];
                        byte digEncAlgoId[MAX_ALGO_SZ];
                        byte signedAttribSet[MAX_SET_SZ];
                            EncodedAttrib signedAttribs[7];
                        byte signerDigest[MAX_OCTET_STR_SZ];
    word32 innerOctetsSz, innerContSeqSz, contentInfoSeqSz;
    word32 outerSeqSz, outerContentSz, innerSeqSz, versionSz, digAlgoIdSetSz,
           singleDigAlgoIdSz, certsSetSz;
    word32 signerInfoSetSz, signerInfoSeqSz, signerVersionSz,
           issuerSnSeqSz, issuerNameSz, issuerSnSz, issuerSKIDSz,
           issuerSKIDSeqSz, signerDigAlgoIdSz, digEncAlgoIdSz, signerDigestSz;
    word32 encContentDigestSz, signedAttribsSz, signedAttribsCount,
           signedAttribSetSz;
} ESD;


static int EncodeAttributes(EncodedAttrib* ea, int eaSz,
                                            PKCS7Attrib* attribs, int attribsSz)
{
    int i;
    int maxSz = min(eaSz, attribsSz);
    int allAttribsSz = 0;

    for (i = 0; i < maxSz; i++)
    {
        int attribSz = 0;

        ea[i].value = attribs[i].value;
        ea[i].valueSz = attribs[i].valueSz;
        attribSz += ea[i].valueSz;
        ea[i].valueSetSz = SetSet(attribSz, ea[i].valueSet);
        attribSz += ea[i].valueSetSz;
        ea[i].oid = attribs[i].oid;
        ea[i].oidSz = attribs[i].oidSz;
        attribSz += ea[i].oidSz;
        ea[i].valueSeqSz = SetSequence(attribSz, ea[i].valueSeq);
        attribSz += ea[i].valueSeqSz;
        ea[i].totalSz = attribSz;

        allAttribsSz += attribSz;
    }
    return allAttribsSz;
}


static int FlattenAttributes(byte* output, EncodedAttrib* ea, int eaSz)
{
    int i, idx;

    idx = 0;
    for (i = 0; i < eaSz; i++) {
        XMEMCPY(output + idx, ea[i].valueSeq, ea[i].valueSeqSz);
        idx += ea[i].valueSeqSz;
        XMEMCPY(output + idx, ea[i].oid, ea[i].oidSz);
        idx += ea[i].oidSz;
        XMEMCPY(output + idx, ea[i].valueSet, ea[i].valueSetSz);
        idx += ea[i].valueSetSz;
        XMEMCPY(output + idx, ea[i].value, ea[i].valueSz);
        idx += ea[i].valueSz;
    }
    return 0;
}


#ifndef NO_RSA

/* returns size of signature put into out, negative on error */
static int wc_PKCS7_RsaSign(PKCS7* pkcs7, byte* in, word32 inSz, ESD* esd)
{
    int ret;
    word32 idx;
#ifdef WOLFSSL_SMALL_STACK
    RsaKey* privKey;
#else
    RsaKey  privKey[1];
#endif

    if (pkcs7 == NULL || pkcs7->rng == NULL || in == NULL || esd == NULL) {
        return BAD_FUNC_ARG;
    }

#ifdef WOLFSSL_SMALL_STACK
    privKey = (RsaKey*)XMALLOC(sizeof(RsaKey), pkcs7->heap,
        DYNAMIC_TYPE_TMP_BUFFER);
    if (privKey == NULL)
        return MEMORY_E;
#endif

    ret = wc_InitRsaKey_ex(privKey, pkcs7->heap, pkcs7->devId);
    if (ret == 0) {
        if (pkcs7->privateKey != NULL && pkcs7->privateKeySz > 0) {
            idx = 0;
            ret = wc_RsaPrivateKeyDecode(pkcs7->privateKey, &idx, privKey,
                                         pkcs7->privateKeySz);
        }
        else if (pkcs7->devId == INVALID_DEVID) {
            ret = BAD_FUNC_ARG;
        }
    }
    if (ret == 0) {
        ret = wc_RsaSSL_Sign(in, inSz, esd->encContentDigest,
                             sizeof(esd->encContentDigest),
                             privKey, pkcs7->rng);
    }

    wc_FreeRsaKey(privKey);
#ifdef WOLFSSL_SMALL_STACK
    XFREE(privKey, pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
#endif

    return ret;
}

#endif /* NO_RSA */


#ifdef HAVE_ECC

/* returns size of signature put into out, negative on error */
static int wc_PKCS7_EcdsaSign(PKCS7* pkcs7, byte* in, word32 inSz, ESD* esd)
{
    int ret;
    word32 outSz, idx;
#ifdef WOLFSSL_SMALL_STACK
    ecc_key* privKey;
#else
    ecc_key  privKey[1];
#endif

    if (pkcs7 == NULL || pkcs7->rng == NULL || in == NULL || esd == NULL) {
        return BAD_FUNC_ARG;
    }

#ifdef WOLFSSL_SMALL_STACK
    privKey = (ecc_key*)XMALLOC(sizeof(ecc_key), pkcs7->heap,
        DYNAMIC_TYPE_TMP_BUFFER);
    if (privKey == NULL)
        return MEMORY_E;
#endif

    ret = wc_ecc_init_ex(privKey, pkcs7->heap, pkcs7->devId);
    if (ret == 0) {
        if (pkcs7->privateKey != NULL && pkcs7->privateKeySz > 0) {
            idx = 0;
            ret = wc_EccPrivateKeyDecode(pkcs7->privateKey, &idx, privKey,
                                         pkcs7->privateKeySz);
        }
        else if (pkcs7->devId == INVALID_DEVID) {
            ret = BAD_FUNC_ARG;
        }
    }
    if (ret == 0) {
        outSz = sizeof(esd->encContentDigest);
        ret = wc_ecc_sign_hash(in, inSz, esd->encContentDigest,
                               &outSz, pkcs7->rng, privKey);
        if (ret == 0)
            ret = (int)outSz;
    }

    wc_ecc_free(privKey);
#ifdef WOLFSSL_SMALL_STACK
    XFREE(privKey, pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
#endif

    return ret;
}

#endif /* HAVE_ECC */


/* builds up SignedData signed attributes, including default ones.
 *
 * pkcs7 - pointer to initialized PKCS7 structure
 * esd   - pointer to initialized ESD structure, used for output
 *
 * return 0 on success, negative on error */
static int wc_PKCS7_BuildSignedAttributes(PKCS7* pkcs7, ESD* esd,
                    const byte* contentType, word32 contentTypeSz,
                    const byte* contentTypeOid, word32 contentTypeOidSz,
                    const byte* messageDigestOid, word32 messageDigestOidSz,
                    const byte* signingTimeOid, word32 signingTimeOidSz,
                    byte* signingTime, word32 signingTimeSz)
{
    int hashSz, timeSz;
    time_t tm;

#ifdef NO_ASN_TIME
    PKCS7Attrib cannedAttribs[2];
#else
    PKCS7Attrib cannedAttribs[3];
#endif
    word32 cannedAttribsCount;

    if (pkcs7 == NULL || esd == NULL || contentType == NULL ||
        contentTypeOid == NULL || messageDigestOid == NULL ||
        signingTimeOid == NULL) {
        return BAD_FUNC_ARG;
    }

    hashSz = wc_HashGetDigestSize(esd->hashType);
    if (hashSz < 0)
        return hashSz;

#ifndef NO_ASN_TIME
    if (signingTime == NULL || signingTimeSz == 0)
        return BAD_FUNC_ARG;

    tm = XTIME(0);
    timeSz = GetAsnTimeString(&tm, signingTime, signingTimeSz);
    if (timeSz < 0)
        return timeSz;
#endif

    cannedAttribsCount = sizeof(cannedAttribs)/sizeof(PKCS7Attrib);

    cannedAttribs[0].oid     = contentTypeOid;
    cannedAttribs[0].oidSz   = contentTypeOidSz;
    cannedAttribs[0].value   = contentType;
    cannedAttribs[0].valueSz = contentTypeSz;
    cannedAttribs[1].oid     = messageDigestOid;
    cannedAttribs[1].oidSz   = messageDigestOidSz;
    cannedAttribs[1].value   = esd->contentDigest;
    cannedAttribs[1].valueSz = hashSz + 2;  /* ASN.1 heading */
#ifndef NO_ASN_TIME
    cannedAttribs[2].oid     = signingTimeOid;
    cannedAttribs[2].oidSz   = signingTimeOidSz;
    cannedAttribs[2].value   = signingTime;
    cannedAttribs[2].valueSz = timeSz;
#endif

    esd->signedAttribsCount += cannedAttribsCount;
    esd->signedAttribsSz += EncodeAttributes(&esd->signedAttribs[0], 3,
                                         cannedAttribs, cannedAttribsCount);

    esd->signedAttribsCount += pkcs7->signedAttribsSz;
#ifdef NO_ASN_TIME
    esd->signedAttribsSz += EncodeAttributes(&esd->signedAttribs[2], 4,
                              pkcs7->signedAttribs, pkcs7->signedAttribsSz);
#else
    esd->signedAttribsSz += EncodeAttributes(&esd->signedAttribs[3], 4,
                              pkcs7->signedAttribs, pkcs7->signedAttribsSz);
#endif

    return 0;
}


/* gets correct encryption algo ID for SignedData, either CTC_<hash>wRSA or
 * CTC_<hash>wECDSA, from pkcs7->publicKeyOID and pkcs7->hashOID.
 *
 * pkcs7          - pointer to PKCS7 structure
 * digEncAlgoId   - [OUT] output int to store correct algo ID in
 * digEncAlgoType - [OUT] output for algo ID type
 *
 * return 0 on success, negative on error */
static int wc_PKCS7_SignedDataGetEncAlgoId(PKCS7* pkcs7, int* digEncAlgoId,
                                           int* digEncAlgoType)
{
    int algoId   = 0;
    int algoType = 0;

    if (pkcs7 == NULL || digEncAlgoId == NULL || digEncAlgoType == NULL)
        return BAD_FUNC_ARG;

    if (pkcs7->publicKeyOID == RSAk) {

        algoType = oidSigType;

        switch (pkcs7->hashOID) {
        #ifndef NO_SHA
            case SHAh:
                algoId = CTC_SHAwRSA;
                break;
        #endif
        #ifdef WOLFSSL_SHA224
            case SHA224h:
                algoId = CTC_SHA224wRSA;
                break;
        #endif
        #ifndef NO_SHA256
            case SHA256h:
                algoId = CTC_SHA256wRSA;
                break;
        #endif
        #ifdef WOLFSSL_SHA384
            case SHA384h:
                algoId = CTC_SHA384wRSA;
                break;
        #endif
        #ifdef WOLFSSL_SHA512
            case SHA512h:
                algoId = CTC_SHA512wRSA;
                break;
        #endif
        }

    }
#ifdef HAVE_ECC
    else if (pkcs7->publicKeyOID == ECDSAk) {

        algoType = oidSigType;

        switch (pkcs7->hashOID) {
        #ifndef NO_SHA
            case SHAh:
                algoId = CTC_SHAwECDSA;
                break;
        #endif
        #ifdef WOLFSSL_SHA224
            case SHA224h:
                algoId = CTC_SHA224wECDSA;
                break;
        #endif
        #ifndef NO_SHA256
            case SHA256h:
                algoId = CTC_SHA256wECDSA;
                break;
        #endif
        #ifdef WOLFSSL_SHA384
            case SHA384h:
                algoId = CTC_SHA384wECDSA;
                break;
        #endif
        #ifdef WOLFSSL_SHA512
            case SHA512h:
                algoId = CTC_SHA512wECDSA;
                break;
        #endif
        }
    }
#endif /* HAVE_ECC */

    if (algoId == 0) {
        WOLFSSL_MSG("Invalid signature algorithm type");
        return BAD_FUNC_ARG;
    }

    *digEncAlgoId = algoId;
    *digEncAlgoType = algoType;

    return 0;
}


/* build SignedData DigestInfo for use with PKCS#7/RSA
 *
 * pkcs7 - pointer to initialized PKCS7 struct
 * flatSignedAttribs - flattened, signed attributes
 * flatSignedAttrbsSz - size of flatSignedAttribs, octets
 * esd - pointer to initialized ESD struct
 * digestInfo - [OUT] output array for DigestInfo
 * digestInfoSz - [IN/OUT] - input size of array, size of digestInfo
 *
 * return 0 on success, negative on error */
static int wc_PKCS7_BuildDigestInfo(PKCS7* pkcs7, byte* flatSignedAttribs,
                                    word32 flatSignedAttribsSz, ESD* esd,
                                    byte* digestInfo, word32* digestInfoSz)
{
    int ret, hashSz, digIdx = 0;
    byte digestInfoSeq[MAX_SEQ_SZ];
    byte digestStr[MAX_OCTET_STR_SZ];
    byte attribSet[MAX_SET_SZ];
    byte algoId[MAX_ALGO_SZ];
    word32 digestInfoSeqSz, digestStrSz, algoIdSz;
    word32 attribSetSz;

    if (pkcs7 == NULL || esd == NULL || digestInfo == NULL ||
        digestInfoSz == NULL) {
        return BAD_FUNC_ARG;
    }

    hashSz = wc_HashGetDigestSize(esd->hashType);
    if (hashSz < 0)
        return hashSz;

    if (pkcs7->signedAttribsSz != 0) {

        if (flatSignedAttribs == NULL)
            return BAD_FUNC_ARG;

        attribSetSz = SetSet(flatSignedAttribsSz, attribSet);

        ret = wc_HashInit(&esd->hash, esd->hashType);
        if (ret < 0)
            return ret;

        ret = wc_HashUpdate(&esd->hash, esd->hashType,
                            attribSet, attribSetSz);
        if (ret == 0)
            ret = wc_HashUpdate(&esd->hash, esd->hashType,
                                flatSignedAttribs, flatSignedAttribsSz);
        if (ret == 0)
            ret = wc_HashFinal(&esd->hash, esd->hashType,
                               esd->contentAttribsDigest);
        wc_HashFree(&esd->hash, esd->hashType);

        if (ret < 0)
            return ret;

    } else {
        /* when no attrs, digest is contentDigest without tag and length */
        XMEMCPY(esd->contentAttribsDigest, esd->contentDigest + 2, hashSz);
    }

    /* set algoID, with NULL attributes */
    algoIdSz = SetAlgoID(pkcs7->hashOID, algoId, oidHashType, 0);

    digestStrSz = SetOctetString(hashSz, digestStr);
    digestInfoSeqSz = SetSequence(algoIdSz + digestStrSz + hashSz,
                                  digestInfoSeq);

    if (*digestInfoSz < (digestInfoSeqSz + algoIdSz + digestStrSz + hashSz)) {
        return BUFFER_E;
    }

    XMEMCPY(digestInfo + digIdx, digestInfoSeq, digestInfoSeqSz);
    digIdx += digestInfoSeqSz;
    XMEMCPY(digestInfo + digIdx, algoId, algoIdSz);
    digIdx += algoIdSz;
    XMEMCPY(digestInfo + digIdx, digestStr, digestStrSz);
    digIdx += digestStrSz;
    XMEMCPY(digestInfo + digIdx, esd->contentAttribsDigest, hashSz);
    digIdx += hashSz;

    *digestInfoSz = digIdx;

    return 0;
}


/* build SignedData signature over DigestInfo or content digest
 *
 * pkcs7 - pointer to initialized PKCS7 struct
 * flatSignedAttribs - flattened, signed attributes
 * flatSignedAttribsSz - size of flatSignedAttribs, octets
 * esd - pointer to initialized ESD struct
 *
 * returns length of signature on success, negative on error */
static int wc_PKCS7_SignedDataBuildSignature(PKCS7* pkcs7,
                                             byte* flatSignedAttribs,
                                             word32 flatSignedAttribsSz,
                                             ESD* esd)
{
    int ret = 0;
#ifdef HAVE_ECC
    int hashSz = 0;
#endif
    word32 digestInfoSz = MAX_PKCS7_DIGEST_SZ;
#ifdef WOLFSSL_SMALL_STACK
    byte* digestInfo;
#else
    byte  digestInfo[MAX_PKCS7_DIGEST_SZ];
#endif

    if (pkcs7 == NULL || esd == NULL)
        return BAD_FUNC_ARG;

#ifdef WOLFSSL_SMALL_STACK
    digestInfo = (byte*)XMALLOC(digestInfoSz, pkcs7->heap,
        DYNAMIC_TYPE_TMP_BUFFER);
    if (digestInfo == NULL) {
        return MEMORY_E;
    }
#endif
    XMEMSET(digestInfo, 0, digestInfoSz);

    ret = wc_PKCS7_BuildDigestInfo(pkcs7, flatSignedAttribs,
                                   flatSignedAttribsSz, esd, digestInfo,
                                   &digestInfoSz);
    if (ret < 0) {
#ifdef WOLFSSL_SMALL_STACK
        XFREE(digestInfo, pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
#endif
        return ret;
    }

    /* sign digestInfo */
    switch (pkcs7->publicKeyOID) {

#ifndef NO_RSA
        case RSAk:
            ret = wc_PKCS7_RsaSign(pkcs7, digestInfo, digestInfoSz, esd);
            break;
#endif

#ifdef HAVE_ECC
        case ECDSAk:
            /* CMS with ECDSA does not sign DigestInfo structure
             * like PKCS#7 with RSA does */
            hashSz = wc_HashGetDigestSize(esd->hashType);
            if (hashSz < 0) {
            #ifdef WOLFSSL_SMALL_STACK
                XFREE(digestInfo, pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
            #endif
                return hashSz;
            }

            ret = wc_PKCS7_EcdsaSign(pkcs7, esd->contentAttribsDigest,
                                     hashSz, esd);
            break;
#endif

        default:
            WOLFSSL_MSG("Unsupported public key type");
            ret = BAD_FUNC_ARG;
    }

#ifdef WOLFSSL_SMALL_STACK
    XFREE(digestInfo, pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
#endif

    if (ret >= 0) {
        esd->encContentDigestSz = (word32)ret;
    }

    return ret;
}


/* build PKCS#7 signedData content type */
static int PKCS7_EncodeSigned(PKCS7* pkcs7, ESD* esd,
    const byte* hashBuf, word32 hashSz, byte* output, word32* outputSz,
    byte* output2, word32* output2Sz)
{
    /* contentType OID (1.2.840.113549.1.9.3) */
    const byte contentTypeOid[] =
            { ASN_OBJECT_ID, 0x09, 0x2a, 0x86, 0x48, 0x86, 0xF7, 0x0d, 0x01,
                             0x09, 0x03 };

    /* messageDigest OID (1.2.840.113549.1.9.4) */
    const byte messageDigestOid[] =
            { ASN_OBJECT_ID, 0x09, 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01,
                             0x09, 0x04 };

    /* signingTime OID () */
    byte signingTimeOid[] =
            { ASN_OBJECT_ID, 0x09, 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01,
                             0x09, 0x05};

    Pkcs7Cert* certPtr = NULL;
    word32 certSetSz = 0;

    word32 signerInfoSz = 0;
    word32 totalSz, total2Sz;
    int idx = 0, ret = 0;
    int digEncAlgoId, digEncAlgoType;
    byte* flatSignedAttribs = NULL;
    word32 flatSignedAttribsSz = 0;

    byte signedDataOid[MAX_OID_SZ];
    word32 signedDataOidSz;

    byte signingTime[MAX_TIME_STRING_SZ];

    if (pkcs7 == NULL || pkcs7->contentSz == 0 ||
        pkcs7->encryptOID == 0 || pkcs7->hashOID == 0 || pkcs7->rng == 0 ||
        output == NULL || outputSz == NULL || *outputSz == 0 || hashSz == 0 ||
        hashBuf == NULL) {
        return BAD_FUNC_ARG;
    }

    /* verify the hash size matches */
#ifdef WOLFSSL_SMALL_STACK
    esd = (ESD*)XMALLOC(sizeof(ESD), pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
    if (esd == NULL)
        return MEMORY_E;
#endif

    XMEMSET(esd, 0, sizeof(ESD));

    /* set content type based on contentOID, unless user has set custom one
       with wc_PKCS7_SetContentType() */
    if (pkcs7->contentTypeSz == 0) {

        /* default to DATA content type if user has not set */
        if (pkcs7->contentOID == 0) {
            pkcs7->contentOID = DATA;
        }

        ret = wc_SetContentType(pkcs7->contentOID, pkcs7->contentType,
                                sizeof(pkcs7->contentType));
        if (ret < 0) {
#ifdef WOLFSSL_SMALL_STACK
        XFREE(esd, pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
#endif
            return ret;
        }
        pkcs7->contentTypeSz = ret;
    }

    /* set signedData outer content type */
    ret = wc_SetContentType(SIGNED_DATA, signedDataOid, sizeof(signedDataOid));
    if (ret < 0) {
#ifdef WOLFSSL_SMALL_STACK
        XFREE(esd, pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
#endif
        return ret;
    }
    signedDataOidSz = ret;

    esd->hashType = wc_OidGetHash(pkcs7->hashOID);
    if (wc_HashGetDigestSize(esd->hashType) != (int)hashSz) {
        WOLFSSL_MSG("hashSz did not match hashOID");
#ifdef WOLFSSL_SMALL_STACK
        XFREE(esd, pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
#endif
        return BUFFER_E;
    }

    /* include hash */
    esd->contentDigest[0] = ASN_OCTET_STRING;
    esd->contentDigest[1] = (byte)hashSz;
    XMEMCPY(&esd->contentDigest[2], hashBuf, hashSz);

    esd->innerOctetsSz = SetOctetString(pkcs7->contentSz, esd->innerOctets);
    esd->innerContSeqSz = SetExplicit(0, esd->innerOctetsSz + pkcs7->contentSz,
                                esd->innerContSeq);
    esd->contentInfoSeqSz = SetSequence(pkcs7->contentSz + esd->innerOctetsSz +
                                     pkcs7->contentTypeSz + esd->innerContSeqSz,
                                     esd->contentInfoSeq);

    /* SignerIdentifier */
    if (pkcs7->sidType == CMS_ISSUER_AND_SERIAL_NUMBER) {
        /* IssuerAndSerialNumber */
        esd->issuerSnSz = SetSerialNumber(pkcs7->issuerSn, pkcs7->issuerSnSz,
                                         esd->issuerSn, MAX_SN_SZ);
        signerInfoSz += esd->issuerSnSz;
        esd->issuerNameSz = SetSequence(pkcs7->issuerSz, esd->issuerName);
        signerInfoSz += esd->issuerNameSz + pkcs7->issuerSz;
        esd->issuerSnSeqSz = SetSequence(signerInfoSz, esd->issuerSnSeq);
        signerInfoSz += esd->issuerSnSeqSz;

        /* version MUST be 1 */
        esd->signerVersionSz = SetMyVersion(1, esd->signerVersion, 0);

    } else if (pkcs7->sidType == CMS_SKID) {
        /* SubjectKeyIdentifier */
        esd->issuerSKIDSz = SetOctetString(KEYID_SIZE, esd->issuerSKID);
        esd->issuerSKIDSeqSz = SetExplicit(0, esd->issuerSKIDSz + KEYID_SIZE,
                                           esd->issuerSKIDSeq);
        signerInfoSz += (esd->issuerSKIDSz + esd->issuerSKIDSeqSz +
                         KEYID_SIZE);

        /* version MUST be 3 */
        esd->signerVersionSz = SetMyVersion(3, esd->signerVersion, 0);
    } else {
    #ifdef WOLFSSL_SMALL_STACK
        XFREE(esd, pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
    #endif
        return SKID_E;
    }

    signerInfoSz += esd->signerVersionSz;
    esd->signerDigAlgoIdSz = SetAlgoID(pkcs7->hashOID, esd->signerDigAlgoId,
                                      oidHashType, 0);
    signerInfoSz += esd->signerDigAlgoIdSz;

    /* set signatureAlgorithm */
    ret = wc_PKCS7_SignedDataGetEncAlgoId(pkcs7, &digEncAlgoId,
                                          &digEncAlgoType);
    if (ret < 0) {
#ifdef WOLFSSL_SMALL_STACK
        XFREE(esd, pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
#endif
        return ret;
    }
    esd->digEncAlgoIdSz = SetAlgoID(digEncAlgoId, esd->digEncAlgoId,
                                    digEncAlgoType, 0);
    signerInfoSz += esd->digEncAlgoIdSz;

    if (pkcs7->signedAttribsSz != 0) {

        /* build up signed attributes */
        ret = wc_PKCS7_BuildSignedAttributes(pkcs7, esd, pkcs7->contentType,
                                     pkcs7->contentTypeSz,
                                     contentTypeOid, sizeof(contentTypeOid),
                                     messageDigestOid, sizeof(messageDigestOid),
                                     signingTimeOid, sizeof(signingTimeOid),
                                     signingTime, sizeof(signingTime));
        if (ret < 0) {
        #ifdef WOLFSSL_SMALL_STACK
            XFREE(esd, pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
        #endif
            return ret;
        }

        flatSignedAttribs = (byte*)XMALLOC(esd->signedAttribsSz, pkcs7->heap,
                                                         DYNAMIC_TYPE_PKCS7);
        flatSignedAttribsSz = esd->signedAttribsSz;
        if (flatSignedAttribs == NULL) {
        #ifdef WOLFSSL_SMALL_STACK
            XFREE(esd, pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
        #endif
            return MEMORY_E;
        }

        FlattenAttributes(flatSignedAttribs,
                                   esd->signedAttribs, esd->signedAttribsCount);
        esd->signedAttribSetSz = SetImplicit(ASN_SET, 0, esd->signedAttribsSz,
                                                          esd->signedAttribSet);
    }

    /* Calculate the final hash and encrypt it. */
    ret = wc_PKCS7_SignedDataBuildSignature(pkcs7, flatSignedAttribs,
                                            flatSignedAttribsSz, esd);
    if (ret < 0) {
        if (pkcs7->signedAttribsSz != 0)
            XFREE(flatSignedAttribs, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
    #ifdef WOLFSSL_SMALL_STACK
        XFREE(esd, pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
    #endif
        return ret;
    }

    signerInfoSz += flatSignedAttribsSz + esd->signedAttribSetSz;

    esd->signerDigestSz = SetOctetString(esd->encContentDigestSz,
                                                             esd->signerDigest);
    signerInfoSz += esd->signerDigestSz + esd->encContentDigestSz;

    esd->signerInfoSeqSz = SetSequence(signerInfoSz, esd->signerInfoSeq);
    signerInfoSz += esd->signerInfoSeqSz;
    esd->signerInfoSetSz = SetSet(signerInfoSz, esd->signerInfoSet);
    signerInfoSz += esd->signerInfoSetSz;

    /* certificates [0] IMPLICIT CertificateSet */
    /* get total certificates size */
    certPtr = pkcs7->certList;
    while (certPtr != NULL) {
        certSetSz += certPtr->derSz;
        certPtr = certPtr->next;
    }
    certPtr = NULL;

    esd->certsSetSz = SetImplicit(ASN_SET, 0, certSetSz, esd->certsSet);

    esd->singleDigAlgoIdSz = SetAlgoID(pkcs7->hashOID, esd->singleDigAlgoId,
                                      oidHashType, 0);
    esd->digAlgoIdSetSz = SetSet(esd->singleDigAlgoIdSz, esd->digAlgoIdSet);


    esd->versionSz = SetMyVersion(1, esd->version, 0);

    totalSz = esd->versionSz + esd->singleDigAlgoIdSz + esd->digAlgoIdSetSz +
              esd->contentInfoSeqSz + pkcs7->contentTypeSz +
              esd->innerContSeqSz + esd->innerOctetsSz + pkcs7->contentSz;
    total2Sz = esd->certsSetSz + certSetSz + signerInfoSz;

    esd->innerSeqSz = SetSequence(totalSz + total2Sz, esd->innerSeq);
    totalSz += esd->innerSeqSz;
    esd->outerContentSz = SetExplicit(0, totalSz + total2Sz, esd->outerContent);
    totalSz += esd->outerContentSz + signedDataOidSz;
    esd->outerSeqSz = SetSequence(totalSz + total2Sz, esd->outerSeq);
    totalSz += esd->outerSeqSz;

    /* if using header/footer, we are not returning the content */
    if (output2 && output2Sz) {
        if (total2Sz > *output2Sz) {
            if (pkcs7->signedAttribsSz != 0)
                XFREE(flatSignedAttribs, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        #ifdef WOLFSSL_SMALL_STACK
            XFREE(esd, pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
        #endif
            return BUFFER_E;
        }
        totalSz -= pkcs7->contentSz;
    }

    if (totalSz > *outputSz) {
        if (pkcs7->signedAttribsSz != 0)
            XFREE(flatSignedAttribs, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
    #ifdef WOLFSSL_SMALL_STACK
        XFREE(esd, pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
    #endif
        return BUFFER_E;
    }

    idx = 0;
    XMEMCPY(output + idx, esd->outerSeq, esd->outerSeqSz);
    idx += esd->outerSeqSz;
    XMEMCPY(output + idx, signedDataOid, signedDataOidSz);
    idx += signedDataOidSz;
    XMEMCPY(output + idx, esd->outerContent, esd->outerContentSz);
    idx += esd->outerContentSz;
    XMEMCPY(output + idx, esd->innerSeq, esd->innerSeqSz);
    idx += esd->innerSeqSz;
    XMEMCPY(output + idx, esd->version, esd->versionSz);
    idx += esd->versionSz;
    XMEMCPY(output + idx, esd->digAlgoIdSet, esd->digAlgoIdSetSz);
    idx += esd->digAlgoIdSetSz;
    XMEMCPY(output + idx, esd->singleDigAlgoId, esd->singleDigAlgoIdSz);
    idx += esd->singleDigAlgoIdSz;
    XMEMCPY(output + idx, esd->contentInfoSeq, esd->contentInfoSeqSz);
    idx += esd->contentInfoSeqSz;
    XMEMCPY(output + idx, pkcs7->contentType, pkcs7->contentTypeSz);
    idx += pkcs7->contentTypeSz;
    XMEMCPY(output + idx, esd->innerContSeq, esd->innerContSeqSz);
    idx += esd->innerContSeqSz;
    XMEMCPY(output + idx, esd->innerOctets, esd->innerOctetsSz);
    idx += esd->innerOctetsSz;

    /* support returning header and footer without content */
    if (output2 && output2Sz) {
        *outputSz = idx;
        idx = 0;
    }
    else {
        XMEMCPY(output + idx, pkcs7->content, pkcs7->contentSz);
        idx += pkcs7->contentSz;
        output2 = output;
    }

    /* certificates */
    XMEMCPY(output2 + idx, esd->certsSet, esd->certsSetSz);
    idx += esd->certsSetSz;
    certPtr = pkcs7->certList;
    while (certPtr != NULL) {
        XMEMCPY(output2 + idx, certPtr->der, certPtr->derSz);
        idx += certPtr->derSz;
        certPtr = certPtr->next;
    }
    wc_PKCS7_FreeCertSet(pkcs7);

    XMEMCPY(output2 + idx, esd->signerInfoSet, esd->signerInfoSetSz);
    idx += esd->signerInfoSetSz;
    XMEMCPY(output2 + idx, esd->signerInfoSeq, esd->signerInfoSeqSz);
    idx += esd->signerInfoSeqSz;
    XMEMCPY(output2 + idx, esd->signerVersion, esd->signerVersionSz);
    idx += esd->signerVersionSz;
    /* SignerIdentifier */
    if (pkcs7->sidType == CMS_ISSUER_AND_SERIAL_NUMBER) {
        /* IssuerAndSerialNumber */
        XMEMCPY(output2 + idx, esd->issuerSnSeq, esd->issuerSnSeqSz);
        idx += esd->issuerSnSeqSz;
        XMEMCPY(output2 + idx, esd->issuerName, esd->issuerNameSz);
        idx += esd->issuerNameSz;
        XMEMCPY(output2 + idx, pkcs7->issuer, pkcs7->issuerSz);
        idx += pkcs7->issuerSz;
        XMEMCPY(output2 + idx, esd->issuerSn, esd->issuerSnSz);
        idx += esd->issuerSnSz;
    } else if (pkcs7->sidType == CMS_SKID) {
        /* SubjectKeyIdentifier */
        XMEMCPY(output2 + idx, esd->issuerSKIDSeq, esd->issuerSKIDSeqSz);
        idx += esd->issuerSKIDSeqSz;
        XMEMCPY(output2 + idx, esd->issuerSKID, esd->issuerSKIDSz);
        idx += esd->issuerSKIDSz;
        XMEMCPY(output2 + idx, pkcs7->issuerSubjKeyId, KEYID_SIZE);
        idx += KEYID_SIZE;
    } else {
    #ifdef WOLFSSL_SMALL_STACK
        XFREE(esd, pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
    #endif
        return SKID_E;
    }
    XMEMCPY(output2 + idx, esd->signerDigAlgoId, esd->signerDigAlgoIdSz);
    idx += esd->signerDigAlgoIdSz;

    /* SignerInfo:Attributes */
    if (flatSignedAttribsSz > 0) {
        XMEMCPY(output2 + idx, esd->signedAttribSet, esd->signedAttribSetSz);
        idx += esd->signedAttribSetSz;
        XMEMCPY(output2 + idx, flatSignedAttribs, flatSignedAttribsSz);
        idx += flatSignedAttribsSz;
        XFREE(flatSignedAttribs, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
    }

    XMEMCPY(output2 + idx, esd->digEncAlgoId, esd->digEncAlgoIdSz);
    idx += esd->digEncAlgoIdSz;
    XMEMCPY(output2 + idx, esd->signerDigest, esd->signerDigestSz);
    idx += esd->signerDigestSz;
    XMEMCPY(output2 + idx, esd->encContentDigest, esd->encContentDigestSz);
    idx += esd->encContentDigestSz;

    if (output2 && output2Sz) {
        *output2Sz = idx;
        idx = 0; /* success */
    }
    else {
        *outputSz = idx;
    }

#ifdef WOLFSSL_SMALL_STACK
    XFREE(esd, pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
#endif
    return idx;
}

/* hashBuf: The computed digest for the pkcs7->content
 * hashSz: The size of computed digest for the pkcs7->content based on hashOID
 * outputHead: The PKCS7 header that goes on top of the raw data signed.
 * outputFoot: The PKCS7 footer that goes at the end of the raw data signed.
 * pkcs7->content: Not used
 * pkcs7->contentSz: Must be provided as actual sign of raw data
 * return codes: 0=success, negative=error
 */
int wc_PKCS7_EncodeSignedData_ex(PKCS7* pkcs7, const byte* hashBuf, word32 hashSz,
    byte* outputHead, word32* outputHeadSz, byte* outputFoot, word32* outputFootSz)
{
    int ret;
#ifdef WOLFSSL_SMALL_STACK
    ESD* esd;
#else
    ESD  esd[1];
#endif

    /* other args checked in wc_PKCS7_EncodeSigned_ex */
    if (pkcs7 == NULL || outputFoot == NULL || outputFootSz == NULL) {
        return BAD_FUNC_ARG;
    }

#ifdef WOLFSSL_SMALL_STACK
    esd = (ESD*)XMALLOC(sizeof(ESD), pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
    if (esd == NULL)
        return MEMORY_E;
#endif

    XMEMSET(esd, 0, sizeof(ESD));

    ret = PKCS7_EncodeSigned(pkcs7, esd, hashBuf, hashSz,
        outputHead, outputHeadSz, outputFoot, outputFootSz);

#ifdef WOLFSSL_SMALL_STACK
    XFREE(esd, pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
#endif

    return ret;
}

/* return codes: >0: Size of signed PKCS7 output buffer, negative: error */
int wc_PKCS7_EncodeSignedData(PKCS7* pkcs7, byte* output, word32 outputSz)
{
    int ret;
    int hashSz;
    enum wc_HashType hashType;
    byte hashBuf[WC_MAX_DIGEST_SIZE];
#ifdef WOLFSSL_SMALL_STACK
    ESD* esd;
#else
    ESD  esd[1];
#endif

    /* other args checked in wc_PKCS7_EncodeSigned_ex */
    if (pkcs7 == NULL || pkcs7->contentSz == 0 || pkcs7->content == NULL) {
        return BAD_FUNC_ARG;
    }

    /* get hash type and size, validate hashOID */
    hashType = wc_OidGetHash(pkcs7->hashOID);
    hashSz = wc_HashGetDigestSize(hashType);
    if (hashSz < 0)
        return hashSz;

#ifdef WOLFSSL_SMALL_STACK
    esd = (ESD*)XMALLOC(sizeof(ESD), pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
    if (esd == NULL)
        return MEMORY_E;
#endif

    XMEMSET(esd, 0, sizeof(ESD));
    esd->hashType = hashType;

    /* calculate hash for content */
    ret = wc_HashInit(&esd->hash, esd->hashType);
    if (ret == 0) {
        ret = wc_HashUpdate(&esd->hash, esd->hashType,
                            pkcs7->content, pkcs7->contentSz);
        if (ret == 0) {
            ret = wc_HashFinal(&esd->hash, esd->hashType, hashBuf);
        }
        wc_HashFree(&esd->hash, esd->hashType);
    }

    if (ret == 0) {
        ret = PKCS7_EncodeSigned(pkcs7, esd, hashBuf, hashSz,
            output, &outputSz, NULL, NULL);
    }

#ifdef WOLFSSL_SMALL_STACK
    XFREE(esd, pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
#endif

    return ret;
}


/* Single-shot API to generate a CMS SignedData bundle that encapsulates a
 * content of type FirmwarePkgData. Any recipient certificates should be
 * loaded into the PKCS7 structure prior to calling this function, using
 * wc_PKCS7_InitWithCert() and/or wc_PKCS7_AddCertificate().
 *
 * pkcs7                - pointer to initialized PKCS7 struct
 * privateKey           - private RSA/ECC key, used for signing SignedData
 * privateKeySz         - size of privateKey, octets
 * signOID              - public key algorithm OID, used for sign operation
 * hashOID              - hash algorithm OID, used for signature generation
 * content              - content to be encapsulated, of type FirmwarePkgData
 * contentSz            - size of content, octets
 * signedAttribs        - optional signed attributes
 * signedAttribsSz      - number of PKCS7Attrib members in signedAttribs
 * output               - output buffer for final bundle
 * outputSz             - size of output buffer, octets
 *
 * Returns length of generated bundle on success, negative upon error. */
int wc_PKCS7_EncodeSignedFPD(PKCS7* pkcs7, byte* privateKey,
                             word32 privateKeySz, int signOID, int hashOID,
                             byte* content, word32 contentSz,
                             PKCS7Attrib* signedAttribs, word32 signedAttribsSz,
                             byte* output, word32 outputSz)
{
    int ret = 0;
    WC_RNG rng;

    if (pkcs7 == NULL || privateKey == NULL || privateKeySz == 0 ||
        content == NULL || contentSz == 0 || output == NULL || outputSz == 0)
        return BAD_FUNC_ARG;

    ret = wc_InitRng(&rng);
    if (ret != 0)
        return ret;

    pkcs7->rng = &rng;
    pkcs7->content = content;
    pkcs7->contentSz = contentSz;
    pkcs7->contentOID = FIRMWARE_PKG_DATA;
    pkcs7->hashOID = hashOID;
    pkcs7->encryptOID = signOID;
    pkcs7->privateKey = privateKey;
    pkcs7->privateKeySz = privateKeySz;
    pkcs7->signedAttribs = signedAttribs;
    pkcs7->signedAttribsSz = signedAttribsSz;

    ret = wc_PKCS7_EncodeSignedData(pkcs7, output, outputSz);
    if (ret <= 0) {
        WOLFSSL_MSG("Error encoding CMS SignedData content type");
        wc_FreeRng(&rng);
        return ret;
    }

    wc_FreeRng(&rng);

    return ret;
}

#ifndef NO_PKCS7_ENCRYPTED_DATA

/* Single-shot API to generate a CMS SignedData bundle that encapsulates a
 * CMS EncryptedData bundle. Content of inner EncryptedData is set to that
 * of FirmwarePkgData. Any recipient certificates should be loaded into the
 * PKCS7 structure prior to calling this function, using wc_PKCS7_InitWithCert()
 * and/or wc_PKCS7_AddCertificate().
 *
 * pkcs7                - pointer to initialized PKCS7 struct
 * encryptKey           - encryption key used for encrypting EncryptedData
 * encryptKeySz         - size of encryptKey, octets
 * privateKey           - private RSA/ECC key, used for signing SignedData
 * privateKeySz         - size of privateKey, octets
 * encryptOID           - encryption algorithm OID, to be used as encryption
 *                        algorithm for EncryptedData
 * signOID              - public key algorithm OID, to be used for sign
 *                        operation in SignedData generation
 * hashOID              - hash algorithm OID, to be used for signature in
 *                        SignedData generation
 * content              - content to be encapsulated
 * contentSz            - size of content, octets
 * unprotectedAttribs   - optional unprotected attributes, for EncryptedData
 * unprotectedAttribsSz - number of PKCS7Attrib members in unprotectedAttribs
 * signedAttribs        - optional signed attributes, for SignedData
 * signedAttribsSz      - number of PKCS7Attrib members in signedAttribs
 * output               - output buffer for final bundle
 * outputSz             - size of output buffer, octets
 *
 * Returns length of generated bundle on success, negative upon error. */
int wc_PKCS7_EncodeSignedEncryptedFPD(PKCS7* pkcs7, byte* encryptKey,
                                      word32 encryptKeySz, byte* privateKey,
                                      word32 privateKeySz, int encryptOID,
                                      int signOID, int hashOID,
                                      byte* content, word32 contentSz,
                                      PKCS7Attrib* unprotectedAttribs,
                                      word32 unprotectedAttribsSz,
                                      PKCS7Attrib* signedAttribs,
                                      word32 signedAttribsSz,
                                      byte* output, word32 outputSz)
{
    int ret = 0, encryptedSz = 0;
    byte* encrypted = NULL;
    WC_RNG rng;

    if (pkcs7 == NULL || encryptKey == NULL || encryptKeySz == 0 ||
        privateKey == NULL || privateKeySz == 0 || content == NULL ||
        contentSz == 0 || output == NULL || outputSz == 0) {
        return BAD_FUNC_ARG;
    }

    /* 1: build up EncryptedData using FirmwarePkgData type, use output
     *    buffer as tmp for storage and to get size */

    /* set struct elements, inner content type is FirmwarePkgData */
    pkcs7->content = content;
    pkcs7->contentSz = contentSz;
    pkcs7->contentOID = FIRMWARE_PKG_DATA;
    pkcs7->encryptOID = encryptOID;
    pkcs7->encryptionKey = encryptKey;
    pkcs7->encryptionKeySz = encryptKeySz;
    pkcs7->unprotectedAttribs = unprotectedAttribs;
    pkcs7->unprotectedAttribsSz = unprotectedAttribsSz;

    encryptedSz = wc_PKCS7_EncodeEncryptedData(pkcs7, output, outputSz);
    if (encryptedSz < 0) {
        WOLFSSL_MSG("Error encoding CMS EncryptedData content type");
        return encryptedSz;
    }

    /* save encryptedData, reset output buffer and struct */
    encrypted = (byte*)XMALLOC(encryptedSz, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
    if (encrypted == NULL) {
        ForceZero(output, outputSz);
        return MEMORY_E;
    }

    XMEMCPY(encrypted, output, encryptedSz);
    ForceZero(output, outputSz);

    ret = wc_InitRng(&rng);
    if (ret != 0) {
        ForceZero(encrypted, encryptedSz);
        XFREE(encrypted, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        return ret;
    }

    /* 2: build up SignedData, encapsulating EncryptedData */
    pkcs7->rng = &rng;
    pkcs7->content = encrypted;
    pkcs7->contentSz = encryptedSz;
    pkcs7->contentOID = ENCRYPTED_DATA;
    pkcs7->hashOID = hashOID;
    pkcs7->encryptOID = signOID;
    pkcs7->privateKey = privateKey;
    pkcs7->privateKeySz = privateKeySz;
    pkcs7->signedAttribs = signedAttribs;
    pkcs7->signedAttribsSz = signedAttribsSz;

    ret = wc_PKCS7_EncodeSignedData(pkcs7, output, outputSz);
    if (ret <= 0) {
        WOLFSSL_MSG("Error encoding CMS SignedData content type");
        ForceZero(encrypted, encryptedSz);
        XFREE(encrypted, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        wc_FreeRng(&rng);
        return ret;
    }

    ForceZero(encrypted, encryptedSz);
    XFREE(encrypted, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
    wc_FreeRng(&rng);

    return ret;
}

#endif /* NO_PKCS7_ENCRYPTED_DATA */

#if defined(HAVE_LIBZ) && !defined(NO_PKCS7_COMPRESSED_DATA)
/* Single-shot API to generate a CMS SignedData bundle that encapsulates a
 * CMS CompressedData bundle. Content of inner CompressedData is set to that
 * of FirmwarePkgData. Any recipient certificates should be loaded into the
 * PKCS7 structure prior to calling this function, using wc_PKCS7_InitWithCert()
 * and/or wc_PKCS7_AddCertificate().
 *
 * pkcs7                - pointer to initialized PKCS7 struct
 * privateKey           - private RSA/ECC key, used for signing SignedData
 * privateKeySz         - size of privateKey, octets
 * signOID              - public key algorithm OID, to be used for sign
 *                        operation in SignedData generation
 * hashOID              - hash algorithm OID, to be used for signature in
 *                        SignedData generation
 * content              - content to be encapsulated
 * contentSz            - size of content, octets
 * signedAttribs        - optional signed attributes, for SignedData
 * signedAttribsSz      - number of PKCS7Attrib members in signedAttribs
 * output               - output buffer for final bundle
 * outputSz             - size of output buffer, octets
 *
 * Returns length of generated bundle on success, negative upon error. */
int wc_PKCS7_EncodeSignedCompressedFPD(PKCS7* pkcs7, byte* privateKey,
                                       word32 privateKeySz, int signOID,
                                       int hashOID, byte* content,
                                       word32 contentSz,
                                       PKCS7Attrib* signedAttribs,
                                       word32 signedAttribsSz, byte* output,
                                       word32 outputSz)
{
    int ret = 0, compressedSz = 0;
    byte* compressed = NULL;
    WC_RNG rng;

    if (pkcs7 == NULL || privateKey == NULL || privateKeySz == 0 ||
        content == NULL || contentSz == 0 || output == NULL || outputSz == 0) {
        return BAD_FUNC_ARG;
    }

    /* 1: build up CompressedData using FirmwarePkgData type, use output
     *    buffer as tmp for storage and to get size */

    /* set struct elements, inner content type is FirmwarePkgData */
    pkcs7->content = content;
    pkcs7->contentSz = contentSz;
    pkcs7->contentOID = FIRMWARE_PKG_DATA;

    compressedSz = wc_PKCS7_EncodeCompressedData(pkcs7, output, outputSz);
    if (compressedSz < 0) {
        WOLFSSL_MSG("Error encoding CMS CompressedData content type");
        return compressedSz;
    }

    /* save compressedData, reset output buffer and struct */
    compressed = (byte*)XMALLOC(compressedSz, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
    if (compressed == NULL) {
        ForceZero(output, outputSz);
        return MEMORY_E;
    }

    XMEMCPY(compressed, output, compressedSz);
    ForceZero(output, outputSz);

    ret = wc_InitRng(&rng);
    if (ret != 0) {
        ForceZero(compressed, compressedSz);
        XFREE(compressed, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        return ret;
    }

    /* 2: build up SignedData, encapsulating EncryptedData */
    pkcs7->rng = &rng;
    pkcs7->content = compressed;
    pkcs7->contentSz = compressedSz;
    pkcs7->contentOID = COMPRESSED_DATA;
    pkcs7->hashOID = hashOID;
    pkcs7->encryptOID = signOID;
    pkcs7->privateKey = privateKey;
    pkcs7->privateKeySz = privateKeySz;
    pkcs7->signedAttribs = signedAttribs;
    pkcs7->signedAttribsSz = signedAttribsSz;

    ret = wc_PKCS7_EncodeSignedData(pkcs7, output, outputSz);
    if (ret <= 0) {
        WOLFSSL_MSG("Error encoding CMS SignedData content type");
        ForceZero(compressed, compressedSz);
        XFREE(compressed, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        wc_FreeRng(&rng);
        return ret;
    }

    ForceZero(compressed, compressedSz);
    XFREE(compressed, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
    wc_FreeRng(&rng);

    return ret;
}

#ifndef NO_PKCS7_ENCRYPTED_DATA

/* Single-shot API to generate a CMS SignedData bundle that encapsulates a
 * CMS EncryptedData bundle, which then encapsulates a CMS CompressedData
 * bundle. Content of inner CompressedData is set to that of FirmwarePkgData.
 * Any recipient certificates should be loaded into the PKCS7 structure prior
 * to calling this function, using wc_PKCS7_InitWithCert() and/or
 * wc_PKCS7_AddCertificate().
 *
 * pkcs7                - pointer to initialized PKCS7 struct
 * encryptKey           - encryption key used for encrypting EncryptedData
 * encryptKeySz         - size of encryptKey, octets
 * privateKey           - private RSA/ECC key, used for signing SignedData
 * privateKeySz         - size of privateKey, octets
 * encryptOID           - encryption algorithm OID, to be used as encryption
 *                        algorithm for EncryptedData
 * signOID              - public key algorithm OID, to be used for sign
 *                        operation in SignedData generation
 * hashOID              - hash algorithm OID, to be used for signature in
 *                        SignedData generation
 * content              - content to be encapsulated
 * contentSz            - size of content, octets
 * unprotectedAttribs   - optional unprotected attributes, for EncryptedData
 * unprotectedAttribsSz - number of PKCS7Attrib members in unprotectedAttribs
 * signedAttribs        - optional signed attributes, for SignedData
 * signedAttribsSz      - number of PKCS7Attrib members in signedAttribs
 * output               - output buffer for final bundle
 * outputSz             - size of output buffer, octets
 *
 * Returns length of generated bundle on success, negative upon error. */
int  wc_PKCS7_EncodeSignedEncryptedCompressedFPD(PKCS7* pkcs7, byte* encryptKey,
                                       word32 encryptKeySz, byte* privateKey,
                                       word32 privateKeySz, int encryptOID,
                                       int signOID, int hashOID, byte* content,
                                       word32 contentSz,
                                       PKCS7Attrib* unprotectedAttribs,
                                       word32 unprotectedAttribsSz,
                                       PKCS7Attrib* signedAttribs,
                                       word32 signedAttribsSz,
                                       byte* output, word32 outputSz)
{
    int ret = 0, compressedSz = 0, encryptedSz = 0;
    byte* compressed = NULL;
    byte* encrypted = NULL;
    WC_RNG rng;

    if (pkcs7 == NULL || encryptKey == NULL || encryptKeySz == 0 ||
        privateKey == NULL || privateKeySz == 0 || content == NULL ||
        contentSz == 0 || output == NULL || outputSz == 0) {
        return BAD_FUNC_ARG;
    }

    /* 1: build up CompressedData using FirmwarePkgData type, use output
     *    buffer as tmp for storage and to get size */
    pkcs7->content = content;
    pkcs7->contentSz = contentSz;
    pkcs7->contentOID = FIRMWARE_PKG_DATA;

    compressedSz = wc_PKCS7_EncodeCompressedData(pkcs7, output, outputSz);
    if (compressedSz < 0) {
        WOLFSSL_MSG("Error encoding CMS CompressedData content type");
        return compressedSz;
    }

    /* save compressedData, reset output buffer and struct */
    compressed = (byte*)XMALLOC(compressedSz, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
    if (compressed == NULL)
        return MEMORY_E;

    XMEMCPY(compressed, output, compressedSz);
    ForceZero(output, outputSz);

    /* 2: build up EncryptedData using CompressedData, use output
     *    buffer as tmp for storage and to get size */
    pkcs7->content = compressed;
    pkcs7->contentSz = compressedSz;
    pkcs7->contentOID = COMPRESSED_DATA;
    pkcs7->encryptOID = encryptOID;
    pkcs7->encryptionKey = encryptKey;
    pkcs7->encryptionKeySz = encryptKeySz;
    pkcs7->unprotectedAttribs = unprotectedAttribs;
    pkcs7->unprotectedAttribsSz = unprotectedAttribsSz;

    encryptedSz = wc_PKCS7_EncodeEncryptedData(pkcs7, output, outputSz);
    if (encryptedSz < 0) {
        WOLFSSL_MSG("Error encoding CMS EncryptedData content type");
        XFREE(compressed, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        return encryptedSz;
    }

    /* save encryptedData, reset output buffer and struct */
    encrypted = (byte*)XMALLOC(encryptedSz, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
    if (encrypted == NULL) {
        ForceZero(compressed, compressedSz);
        XFREE(compressed, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        return MEMORY_E;
    }

    XMEMCPY(encrypted, output, encryptedSz);
    ForceZero(compressed, compressedSz);
    XFREE(compressed, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
    ForceZero(output, outputSz);

    ret = wc_InitRng(&rng);
    if (ret != 0) {
        ForceZero(encrypted, encryptedSz);
        XFREE(encrypted, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        return ret;
    }

    /* 3: build up SignedData, encapsulating EncryptedData */
    pkcs7->rng = &rng;
    pkcs7->content = encrypted;
    pkcs7->contentSz = encryptedSz;
    pkcs7->contentOID = ENCRYPTED_DATA;
    pkcs7->hashOID = hashOID;
    pkcs7->encryptOID = signOID;
    pkcs7->privateKey = privateKey;
    pkcs7->privateKeySz = privateKeySz;
    pkcs7->signedAttribs = signedAttribs;
    pkcs7->signedAttribsSz = signedAttribsSz;

    ret = wc_PKCS7_EncodeSignedData(pkcs7, output, outputSz);
    if (ret <= 0) {
        WOLFSSL_MSG("Error encoding CMS SignedData content type");
        ForceZero(encrypted, encryptedSz);
        XFREE(encrypted, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        wc_FreeRng(&rng);
        return ret;
    }

    ForceZero(encrypted, encryptedSz);
    XFREE(encrypted, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
    wc_FreeRng(&rng);

    return ret;
}

#endif /* !NO_PKCS7_ENCRYPTED_DATA */
#endif /* HAVE_LIBZ && !NO_PKCS7_COMPRESSED_DATA */


#ifndef NO_RSA

/* returns size of signature put into out, negative on error */
static int wc_PKCS7_RsaVerify(PKCS7* pkcs7, byte* sig, int sigSz,
                              byte* hash, word32 hashSz)
{
    int ret = 0, i;
    word32 scratch = 0, verified = 0;
#ifdef WOLFSSL_SMALL_STACK
    byte* digest;
    RsaKey* key;
    DecodedCert* dCert;
#else
    byte digest[MAX_PKCS7_DIGEST_SZ];
    RsaKey key[1];
    DecodedCert stack_dCert;
    DecodedCert* dCert = &stack_dCert;
#endif

    if (pkcs7 == NULL || sig == NULL || hash == NULL) {
        return BAD_FUNC_ARG;
    }

#ifdef WOLFSSL_SMALL_STACK
    digest = (byte*)XMALLOC(MAX_PKCS7_DIGEST_SZ, pkcs7->heap,
                            DYNAMIC_TYPE_TMP_BUFFER);
    if (digest == NULL)
        return MEMORY_E;

    key = (RsaKey*)XMALLOC(sizeof(RsaKey), pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
    if (key == NULL) {
        XFREE(digest, pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
        return MEMORY_E;
    }

    dCert = (DecodedCert*)XMALLOC(sizeof(DecodedCert), pkcs7->heap,
                                  DYNAMIC_TYPE_DCERT);
    if (dCert == NULL) {
        XFREE(digest, pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
        XFREE(key, pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
        return MEMORY_E;
    }
#endif

    XMEMSET(digest, 0, MAX_PKCS7_DIGEST_SZ);

    /* loop over certs received in certificates set, try to find one
     * that will validate signature */
    for (i = 0; i < MAX_PKCS7_CERTS; i++) {

        verified = 0;
        scratch  = 0;

        if (pkcs7->certSz[i] == 0)
            continue;

        ret = wc_InitRsaKey_ex(key, pkcs7->heap, pkcs7->devId);
        if (ret != 0) {
#ifdef WOLFSSL_SMALL_STACK
            XFREE(digest, pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
            XFREE(key,    pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
            XFREE(dCert,  pkcs7->heap, DYNAMIC_TYPE_DCERT);
#endif
            return ret;
        }

        InitDecodedCert(dCert, pkcs7->cert[i], pkcs7->certSz[i], pkcs7->heap);
        /* not verifying, only using this to extract public key */
        ret = ParseCert(dCert, CA_TYPE, NO_VERIFY, 0);
        if (ret < 0) {
            WOLFSSL_MSG("ASN RSA cert parse error");
            FreeDecodedCert(dCert);
            wc_FreeRsaKey(key);
            continue;
        }

        if (wc_RsaPublicKeyDecode(dCert->publicKey, &scratch, key,
                                  dCert->pubKeySize) < 0) {
            WOLFSSL_MSG("ASN RSA key decode error");
            FreeDecodedCert(dCert);
            wc_FreeRsaKey(key);
            continue;
        }

        ret = wc_RsaSSL_Verify(sig, sigSz, digest, MAX_PKCS7_DIGEST_SZ, key);
        FreeDecodedCert(dCert);
        wc_FreeRsaKey(key);

        if ((ret > 0) && (hashSz == (word32)ret)) {
            if (XMEMCMP(digest, hash, hashSz) == 0) {
                /* found signer that successfully verified signature */
                verified = 1;
                break;
            }
        }
    }

    if (verified == 0) {
        ret = SIG_VERIFY_E;
    }

#ifdef WOLFSSL_SMALL_STACK
    XFREE(digest, pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
    XFREE(key,    pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
    XFREE(dCert,  pkcs7->heap, DYNAMIC_TYPE_DCERT);
#endif

    return ret;
}

#endif /* NO_RSA */


#ifdef HAVE_ECC

/* returns size of signature put into out, negative on error */
static int wc_PKCS7_EcdsaVerify(PKCS7* pkcs7, byte* sig, int sigSz,
                                byte* hash, word32 hashSz)
{
    int ret = 0, i;
    int res = 0;
    int verified = 0;
#ifdef WOLFSSL_SMALL_STACK
    byte* digest;
    ecc_key* key;
    DecodedCert* dCert;
#else
    byte digest[MAX_PKCS7_DIGEST_SZ];
    ecc_key key[1];
    DecodedCert stack_dCert;
    DecodedCert* dCert = &stack_dCert;
#endif
    word32 idx = 0;

    if (pkcs7 == NULL || sig == NULL)
        return BAD_FUNC_ARG;

#ifdef WOLFSSL_SMALL_STACK
    digest = (byte*)XMALLOC(MAX_PKCS7_DIGEST_SZ, pkcs7->heap,
                            DYNAMIC_TYPE_TMP_BUFFER);
    if (digest == NULL)
        return MEMORY_E;

    key = (ecc_key*)XMALLOC(sizeof(ecc_key), pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
    if (key == NULL) {
        XFREE(digest, pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
        return MEMORY_E;
    }

    dCert = (DecodedCert*)XMALLOC(sizeof(DecodedCert), pkcs7->heap,
                                  DYNAMIC_TYPE_DCERT);
    if (dCert == NULL) {
        XFREE(digest, pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
        XFREE(key,    pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
        return MEMORY_E;
    }
#endif

    XMEMSET(digest, 0, MAX_PKCS7_DIGEST_SZ);

    /* loop over certs received in certificates set, try to find one
     * that will validate signature */
    for (i = 0; i < MAX_PKCS7_CERTS; i++) {

        verified = 0;

        if (pkcs7->certSz[i] == 0)
            continue;

        ret = wc_ecc_init_ex(key, pkcs7->heap, pkcs7->devId);
        if (ret != 0) {
#ifdef WOLFSSL_SMALL_STACK
            XFREE(digest, pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
            XFREE(key,    pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
            XFREE(dCert,  pkcs7->heap, DYNAMIC_TYPE_DCERT);
#endif
            return ret;
        }

        InitDecodedCert(dCert, pkcs7->cert[i], pkcs7->certSz[i], pkcs7->heap);
        /* not verifying, only using this to extract public key */
        ret = ParseCert(dCert, CA_TYPE, NO_VERIFY, 0);
        if (ret < 0) {
            WOLFSSL_MSG("ASN ECC cert parse error");
            FreeDecodedCert(dCert);
            wc_ecc_free(key);
            continue;
        }

        if (wc_EccPublicKeyDecode(pkcs7->publicKey, &idx, key,
                                  pkcs7->publicKeySz) < 0) {
            WOLFSSL_MSG("ASN ECC key decode error");
            FreeDecodedCert(dCert);
            wc_ecc_free(key);
            continue;
        }

        ret = wc_ecc_verify_hash(sig, sigSz, hash, hashSz, &res, key);

        FreeDecodedCert(dCert);
        wc_ecc_free(key);

        if (ret == 0 && res == 1) {
            /* found signer that successfully verified signature */
            verified = 1;
            break;
        }
    }

    if (verified == 0) {
        ret = SIG_VERIFY_E;
    }

#ifdef WOLFSSL_SMALL_STACK
    XFREE(digest, pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
    XFREE(key,    pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
    XFREE(dCert,  pkcs7->heap, DYNAMIC_TYPE_DCERT);
#endif

    return ret;
}

#endif /* HAVE_ECC */


/* build SignedData digest, both in PKCS#7 DigestInfo format and
 * as plain digest for CMS.
 *
 * pkcs7          - pointer to initialized PKCS7 struct
 * signedAttrib   - signed attributes
 * signedAttribSz - size of signedAttrib, octets
 * pkcs7Digest    - [OUT] PKCS#7 DigestInfo
 * pkcs7DigestSz  - [IN/OUT] size of pkcs7Digest
 * plainDigest    - [OUT] pointer to plain digest, offset into pkcs7Digest
 * plainDigestSz  - [OUT] size of digest at plainDigest
 *
 * returns 0 on success, negative on error */
static int wc_PKCS7_BuildSignedDataDigest(PKCS7* pkcs7, byte* signedAttrib,
                                      word32 signedAttribSz, byte* pkcs7Digest,
                                      word32* pkcs7DigestSz, byte** plainDigest,
                                      word32* plainDigestSz,
                                      const byte* hashBuf, word32 hashBufSz)
{
    int ret = 0, digIdx = 0;
    word32 attribSetSz = 0, hashSz = 0;
    byte attribSet[MAX_SET_SZ];
    byte digest[WC_MAX_DIGEST_SIZE];
    byte digestInfoSeq[MAX_SEQ_SZ];
    byte digestStr[MAX_OCTET_STR_SZ];
    byte algoId[MAX_ALGO_SZ];
    word32 digestInfoSeqSz, digestStrSz, algoIdSz;
#ifdef WOLFSSL_SMALL_STACK
    byte* digestInfo;
#else
    byte  digestInfo[MAX_PKCS7_DIGEST_SZ];
#endif

    wc_HashAlg hash;
    enum wc_HashType hashType;

    /* check arguments */
    if (pkcs7 == NULL || pkcs7Digest == NULL ||
        pkcs7DigestSz == NULL || plainDigest == NULL) {
        return BAD_FUNC_ARG;
    }

    hashType = wc_OidGetHash(pkcs7->hashOID);
    ret = wc_HashGetDigestSize(hashType);
    if (ret < 0)
        return ret;
    hashSz = ret;

    if (signedAttribSz > 0) {
        if (signedAttrib == NULL)
            return BAD_FUNC_ARG;
    }
    else {
        if (hashBuf && hashBufSz > 0) {
            if (hashSz != hashBufSz)
                return BAD_FUNC_ARG;
        }
        else if (pkcs7->content == NULL)
            return BAD_FUNC_ARG;
    }

#ifdef WOLFSSL_SMALL_STACK
    digestInfo = (byte*)XMALLOC(MAX_PKCS7_DIGEST_SZ, pkcs7->heap,
        DYNAMIC_TYPE_TMP_BUFFER);
    if (digestInfo == NULL)
        return MEMORY_E;
#endif

    XMEMSET(pkcs7Digest, 0, *pkcs7DigestSz);
    XMEMSET(digest,      0, WC_MAX_DIGEST_SIZE);
    XMEMSET(digestInfo,  0, MAX_PKCS7_DIGEST_SZ);


    /* calculate digest */
    if (hashBuf && hashBufSz > 0 && signedAttribSz == 0) {
        XMEMCPY(digest, hashBuf, hashBufSz);
    }
    else {
        ret = wc_HashInit(&hash, hashType);
        if (ret < 0) {
    #ifdef WOLFSSL_SMALL_STACK
            XFREE(digestInfo, pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
    #endif
            return ret;
        }

        if (signedAttribSz > 0) {
            attribSetSz = SetSet(signedAttribSz, attribSet);

            /* calculate digest */
            ret = wc_HashUpdate(&hash, hashType, attribSet, attribSetSz);
            if (ret == 0)
                ret = wc_HashUpdate(&hash, hashType, signedAttrib, signedAttribSz);
            if (ret == 0)
                ret = wc_HashFinal(&hash, hashType, digest);
        } else {
            ret = wc_HashUpdate(&hash, hashType, pkcs7->content, pkcs7->contentSz);
            if (ret == 0)
                ret = wc_HashFinal(&hash, hashType, digest);
        }

        wc_HashFree(&hash, hashType);
        if (ret < 0) {
    #ifdef WOLFSSL_SMALL_STACK
            XFREE(digestInfo, pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
    #endif
            return ret;
        }
    }

    /* Set algoID, with NULL attributes */
    algoIdSz = SetAlgoID(pkcs7->hashOID, algoId, oidHashType, 0);

    digestStrSz = SetOctetString(hashSz, digestStr);
    digestInfoSeqSz = SetSequence(algoIdSz + digestStrSz + hashSz,
                                  digestInfoSeq);

    XMEMCPY(digestInfo + digIdx, digestInfoSeq, digestInfoSeqSz);
    digIdx += digestInfoSeqSz;
    XMEMCPY(digestInfo + digIdx, algoId, algoIdSz);
    digIdx += algoIdSz;
    XMEMCPY(digestInfo + digIdx, digestStr, digestStrSz);
    digIdx += digestStrSz;
    XMEMCPY(digestInfo + digIdx, digest, hashSz);
    digIdx += hashSz;

    XMEMCPY(pkcs7Digest, digestInfo, digIdx);
    *pkcs7DigestSz = digIdx;

    /* set plain digest pointer */
    *plainDigest = pkcs7Digest + digIdx - hashSz;
    *plainDigestSz = hashSz;

#ifdef WOLFSSL_SMALL_STACK
    XFREE(digestInfo, pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
#endif
    return 0;
}


/* verifies SignedData signature, over either PKCS#7 DigestInfo or
 * content digest.
 *
 * pkcs7          - pointer to initialized PKCS7 struct
 * sig            - signature to verify
 * sigSz          - size of sig
 * signedAttrib   - signed attributes, or null if empty
 * signedAttribSz - size of signedAttributes
 *
 * return 0 on success, negative on error */
static int wc_PKCS7_SignedDataVerifySignature(PKCS7* pkcs7, byte* sig,
                                             word32 sigSz, byte* signedAttrib,
                                             word32 signedAttribSz,
                                             const byte* hashBuf, word32 hashSz)
{
    int ret = 0;
    word32 plainDigestSz = 0, pkcs7DigestSz;
    byte* plainDigest = NULL; /* offset into pkcs7Digest */
#ifdef WOLFSSL_SMALL_STACK
    byte* pkcs7Digest;
#else
    byte  pkcs7Digest[MAX_PKCS7_DIGEST_SZ];
#endif

    if (pkcs7 == NULL)
        return BAD_FUNC_ARG;

    /* build hash to verify against */
    pkcs7DigestSz = MAX_PKCS7_DIGEST_SZ;
#ifdef WOLFSSL_SMALL_STACK
    pkcs7Digest = (byte*)XMALLOC(pkcs7DigestSz, pkcs7->heap,
                                 DYNAMIC_TYPE_TMP_BUFFER);
    if (pkcs7Digest == NULL)
        return MEMORY_E;
#endif

    XMEMSET(pkcs7Digest, 0, pkcs7DigestSz);

    ret = wc_PKCS7_BuildSignedDataDigest(pkcs7, signedAttrib,
                                         signedAttribSz, pkcs7Digest,
                                         &pkcs7DigestSz, &plainDigest,
                                         &plainDigestSz, hashBuf, hashSz);
    if (ret < 0) {
#ifdef WOLFSSL_SMALL_STACK
        XFREE(pkcs7Digest, pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
#endif
        return ret;
    }

    switch (pkcs7->publicKeyOID) {

#ifndef NO_RSA
        case RSAk:
            ret = wc_PKCS7_RsaVerify(pkcs7, sig, sigSz, pkcs7Digest,
                                     pkcs7DigestSz);
            if (ret < 0) {
                WOLFSSL_MSG("PKCS#7 verification failed, trying CMS");
                ret = wc_PKCS7_RsaVerify(pkcs7, sig, sigSz, plainDigest,
                                         plainDigestSz);
            }
            break;
#endif

#ifdef HAVE_ECC
        case ECDSAk:
            ret = wc_PKCS7_EcdsaVerify(pkcs7, sig, sigSz, plainDigest,
                                       plainDigestSz);
            break;
#endif

        default:
            WOLFSSL_MSG("Unsupported public key type");
            ret = BAD_FUNC_ARG;
    }

#ifdef WOLFSSL_SMALL_STACK
     XFREE(pkcs7Digest, pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
#endif
    return ret;
}


/* set correct public key OID based on signature OID, stores in
 * pkcs7->publicKeyOID and returns same value */
static int wc_PKCS7_SetPublicKeyOID(PKCS7* pkcs7, int sigOID)
{
    if (pkcs7 == NULL)
        return BAD_FUNC_ARG;

    pkcs7->publicKeyOID = 0;

    switch (sigOID) {

    #ifndef NO_RSA
        /* RSA signature types */
        case CTC_MD2wRSA:
        case CTC_MD5wRSA:
        case CTC_SHAwRSA:
        case CTC_SHA224wRSA:
        case CTC_SHA256wRSA:
        case CTC_SHA384wRSA:
        case CTC_SHA512wRSA:
            pkcs7->publicKeyOID = RSAk;
            break;

        /* if sigOID is already RSAk */
        case RSAk:
            pkcs7->publicKeyOID = sigOID;
            break;
    #endif

    #ifndef NO_DSA
        /* DSA signature types */
        case CTC_SHAwDSA:
            pkcs7->publicKeyOID = DSAk;
            break;

        /* if sigOID is already DSAk */
        case DSAk:
            pkcs7->publicKeyOID = sigOID;
            break;
    #endif

    #ifdef HAVE_ECC
        /* ECDSA signature types */
        case CTC_SHAwECDSA:
        case CTC_SHA224wECDSA:
        case CTC_SHA256wECDSA:
        case CTC_SHA384wECDSA:
        case CTC_SHA512wECDSA:
            pkcs7->publicKeyOID = ECDSAk;
            break;

        /* if sigOID is already ECDSAk */
        case ECDSAk:
            pkcs7->publicKeyOID = sigOID;
            break;
    #endif

        default:
            WOLFSSL_MSG("Unsupported public key algorithm");
            return ASN_SIG_KEY_E;
    }

    return pkcs7->publicKeyOID;
}


/* Parses through the attributes and adds them to the PKCS7 structure
 * Creates dynamic attribute structures that are free'd with calling
 * wc_PKCS7_Free()
 *
 * NOTE: An attribute has the ASN1 format of
 ** Sequence
 ****** Object ID
 ****** Set
 ********** {PritnableString, UTCTime, OCTET STRING ...}
 *
 * pkcs7  the PKCS7 structure to put the parsed attributes into
 * in     buffer holding all attributes
 * inSz   size of in buffer
 *
 * returns the number of attributes parsed on success
 */
static int wc_PKCS7_ParseAttribs(PKCS7* pkcs7, byte* in, int inSz)
{
    int    found = 0;
    word32 idx   = 0;
    word32 oid;

    if (pkcs7 == NULL || in == NULL || inSz < 0) {
        return BAD_FUNC_ARG;
    }

    while (idx < (word32)inSz) {
        int length  = 0;
        int oidIdx;
        PKCS7DecodedAttrib* attrib;

        if (GetSequence(in, &idx, &length, inSz) < 0)
            return ASN_PARSE_E;

        attrib = (PKCS7DecodedAttrib*)XMALLOC(sizeof(PKCS7DecodedAttrib),
                pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        if (attrib == NULL) {
            return MEMORY_E;
        }
        XMEMSET(attrib, 0, sizeof(PKCS7DecodedAttrib));

        oidIdx = idx;
        if (GetObjectId(in, &idx, &oid, oidIgnoreType, inSz)
                < 0) {
            XFREE(attrib, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
            return ASN_PARSE_E;
        }
        attrib->oidSz = idx - oidIdx;
        attrib->oid = (byte*)XMALLOC(attrib->oidSz, pkcs7->heap,
                                     DYNAMIC_TYPE_PKCS7);
        if (attrib->oid == NULL) {
            XFREE(attrib, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
            return MEMORY_E;
        }
        XMEMCPY(attrib->oid, in + oidIdx, attrib->oidSz);


        /* Get Set that contains the printable string value */
        if (GetSet(in, &idx, &length, inSz) < 0) {
            XFREE(attrib->oid, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
            XFREE(attrib, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
            return ASN_PARSE_E;
        }

        if ((inSz - idx) < (word32)length) {
            XFREE(attrib->oid, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
            XFREE(attrib, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
            return ASN_PARSE_E;
        }

        attrib->valueSz = (word32)length;
        attrib->value = (byte*)XMALLOC(attrib->valueSz, pkcs7->heap,
                                       DYNAMIC_TYPE_PKCS7);
        if (attrib->value == NULL) {
            XFREE(attrib->oid, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
            XFREE(attrib, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
            return MEMORY_E;
        }
        XMEMCPY(attrib->value, in + idx, attrib->valueSz);
        idx += length;

        /* store attribute in linked list */
        if (pkcs7->decodedAttrib != NULL) {
            attrib->next = pkcs7->decodedAttrib;
            pkcs7->decodedAttrib = attrib;
        } else {
            pkcs7->decodedAttrib = attrib;
        }
        found++;
    }

    return found;
}


/* option to turn off support for degenerate cases
 * flag 0 turns off support
 * flag 1 turns on support
 *
 * by default support for SignedData degenerate cases is on
 */
void wc_PKCS7_AllowDegenerate(PKCS7* pkcs7, word16 flag)
{
    if (pkcs7) {
        if (flag) { /* flag of 1 turns on support for degenerate */
            pkcs7->noDegenerate = 0;
        }
        else { /* flag of 0 turns off support */
            pkcs7->noDegenerate = 1;
        }
    }
}

/* Finds the certificates in the message and saves it. By default allows
 * degenerate cases which can have no signer.
 *
 * By default expects type SIGNED_DATA (SignedData) which can have any number of
 * elements in signerInfos collection, inluding zero. (RFC2315 section 9.1)
 * When adding support for the case of SignedAndEnvelopedData content types a
 * signer is required. In this case the PKCS7 flag noDegenerate could be set.
 */
static int PKCS7_VerifySignedData(PKCS7* pkcs7, const byte* hashBuf,
    word32 hashSz, byte* in, word32 inSz,
    byte* in2, word32 in2Sz)
{
    word32 idx, outerContentType, hashOID = 0, sigOID, contentTypeSz = 0, totalSz = 0;
    int length, version, ret = 0;
    byte* content = NULL;
    byte* contentDynamic = NULL;
    byte* sig = NULL;
    byte* cert = NULL;
    byte* signedAttrib = NULL;
    byte* contentType = NULL;
    int contentSz = 0, sigSz = 0, certSz = 0, signedAttribSz = 0;
    word32 localIdx, start;
    byte degenerate = 0;
#ifdef ASN_BER_TO_DER
    byte* der;
#endif
    int multiPart = 0, keepContent;
    int contentLen;

    byte* pkiMsg    = in;
    word32 pkiMsgSz = inSz;
#ifndef NO_PKCS7_STREAM
    word32 stateIdx = 0;
#endif

    byte* pkiMsg2 = in2;
    word32 pkiMsg2Sz = in2Sz;

    if (pkcs7 == NULL)
        return BAD_FUNC_ARG;

#ifndef NO_PKCS7_STREAM
    /* allow for 0 size inputs with stream mode */
    if (pkcs7 == NULL || (pkiMsg == NULL && pkiMsgSz > 0))
        return BAD_FUNC_ARG;

#else
    if (pkiMsg == NULL || pkiMsgSz == 0)
        return BAD_FUNC_ARG;

#endif

    if ((hashSz > 0 && hashBuf == NULL) || (pkiMsg2Sz > 0 && pkiMsg2 == NULL)) {
        return BAD_FUNC_ARG;
    }
    idx = 0;

#ifndef NO_PKCS7_STREAM
    if (pkcs7->stream == NULL) {
        if ((ret = wc_PKCS7_CreateStream(pkcs7)) != 0) {
            return ret;
        }
    }
#endif

    switch (pkcs7->state) {
        case WC_PKCS7_START:
        #ifndef NO_PKCS7_STREAM
            if ((ret = wc_PKCS7_AddDataToStream(pkcs7, in, inSz, MAX_SEQ_SZ +
                            MAX_VERSION_SZ + MAX_SEQ_SZ + MAX_LENGTH_SZ +
                            ASN_TAG_SZ + MAX_OID_SZ + MAX_SEQ_SZ,
                            &pkiMsg, &idx)) != 0) {
                break;
            }

            pkiMsgSz = wc_PKCS7_GetMaxStream(pkcs7, PKCS7_SEQ_PEEK, in, inSz);
        #endif

            /* determine total message size */
            totalSz = pkiMsgSz;
            if (pkiMsg2 && pkiMsg2Sz > 0) {
                totalSz += pkiMsg2Sz + pkcs7->contentSz;
            }

            /* Get the contentInfo sequence */
            if (ret == 0 && GetSequence(pkiMsg, &idx, &length, totalSz) < 0)
                ret = ASN_PARSE_E;

            if (ret == 0 && length == 0 && pkiMsg[idx-1] == 0x80) {
        #ifdef ASN_BER_TO_DER
                word32 len = 0;

                ret = wc_BerToDer(pkiMsg, pkiMsgSz, NULL, &len);
                if (ret != LENGTH_ONLY_E)
                    return ret;
                pkcs7->der = (byte*)XMALLOC(len, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
                if (pkcs7->der == NULL)
                    return MEMORY_E;
                ret = wc_BerToDer(pkiMsg, pkiMsgSz, pkcs7->der, &len);
                if (ret < 0)
                    return ret;

                pkiMsg = pkcs7->der;
                pkiMsgSz = len;
                idx = 0;
                if (GetSequence(pkiMsg, &idx, &length, pkiMsgSz) < 0)
                    return ASN_PARSE_E;
        #else
                ret = BER_INDEF_E;
        #endif
            }

            /* Get the contentInfo contentType */
            if (ret == 0 && wc_GetContentType(pkiMsg, &idx, &outerContentType,
                        pkiMsgSz) < 0)
                ret = ASN_PARSE_E;

            if (ret == 0 && outerContentType != SIGNED_DATA) {
                WOLFSSL_MSG("PKCS#7 input not of type SignedData");
                ret = PKCS7_OID_E;
            }

            /* get the ContentInfo content */
            if (ret == 0 && pkiMsg[idx++] !=
                    (ASN_CONSTRUCTED | ASN_CONTEXT_SPECIFIC | 0))
                ret = ASN_PARSE_E;

            if (ret == 0 && GetLength(pkiMsg, &idx, &length, totalSz) < 0)
                ret = ASN_PARSE_E;

            /* Get the signedData sequence */
            if (ret == 0 && GetSequence(pkiMsg, &idx, &length, totalSz) < 0)
                ret = ASN_PARSE_E;

            /* Get the version */
            if (ret == 0 && GetMyVersion(pkiMsg, &idx, &version, pkiMsgSz) < 0)
                ret = ASN_PARSE_E;

            if (ret == 0 && version != 1) {
                WOLFSSL_MSG("PKCS#7 signedData needs to be of version 1");
                ret = ASN_VERSION_E;
            }

            /* Get the set of DigestAlgorithmIdentifiers */
            if (ret == 0 && GetSet(pkiMsg, &idx, &length, pkiMsgSz) < 0)
                ret = ASN_PARSE_E;

            /* Skip the set. */
            idx += length;
            degenerate = (length == 0)? 1 : 0;
            if (pkcs7->noDegenerate == 1 && degenerate == 1) {
                ret = PKCS7_NO_SIGNER_E;
            }

            if (ret != 0)
                break;

        #ifndef NO_PKCS7_STREAM
            if ((ret = wc_PKCS7_StreamEndCase(pkcs7, &stateIdx, &idx)) != 0) {
                break;
            }
            pkcs7->stream->maxLen = totalSz;
            wc_PKCS7_StreamStoreVar(pkcs7, totalSz, 0, 0);
        #endif

            wc_PKCS7_ChangeState(pkcs7, WC_PKCS7_VERIFY_STAGE2);
            FALL_THROUGH;

        case WC_PKCS7_VERIFY_STAGE2:
        #ifndef NO_PKCS7_STREAM
            if ((ret = wc_PKCS7_AddDataToStream(pkcs7, in, inSz + in2Sz,
                           MAX_SEQ_SZ + MAX_OID_SZ + ASN_TAG_SZ + MAX_LENGTH_SZ
                           + ASN_TAG_SZ + MAX_LENGTH_SZ, &pkiMsg, &idx)) != 0) {
                break;
            }

            pkiMsgSz = wc_PKCS7_GetMaxStream(pkcs7, PKCS7_DEFAULT_PEEK, in, inSz);
            wc_PKCS7_StreamGetVar(pkcs7, &totalSz, 0, 0);
        #endif

            /* Get the inner ContentInfo sequence */
            if (GetSequence(pkiMsg, &idx, &length, totalSz) < 0)
                ret = ASN_PARSE_E;

            /* Get the inner ContentInfo contentType */
            if (ret == 0) {
                word32 tmpIdx = idx;

                if (GetASNObjectId(pkiMsg, &idx, &length, pkiMsgSz) != 0)
                    ret = ASN_PARSE_E;

                contentType = pkiMsg + tmpIdx;
                contentTypeSz = length + (idx - tmpIdx);

                idx += length;
            }

            if (ret != 0)
                break;

            /* Check for content info, it could be omitted when degenerate */
            localIdx = idx;
            ret = 0;
            if (pkiMsg[localIdx++] != (ASN_CONSTRUCTED | ASN_CONTEXT_SPECIFIC | 0))
                ret = ASN_PARSE_E;

            if (ret == 0 && GetLength(pkiMsg, &localIdx, &length, totalSz) <= 0)
                ret = ASN_PARSE_E;


            /* get length of content in the case that there is multiple parts */
            if (ret == 0 && pkiMsg[localIdx] == (ASN_OCTET_STRING | ASN_CONSTRUCTED)) {
                multiPart = 1;

                localIdx++;
                /* Get length of all OCTET_STRINGs. */
                if (GetLength(pkiMsg, &localIdx, &contentLen, totalSz) < 0)
                    ret = ASN_PARSE_E;

                /* Check whether there is one OCTET_STRING inside. */
                start = localIdx;
                if (ret == 0 && pkiMsg[localIdx++] != ASN_OCTET_STRING)
                    ret = ASN_PARSE_E;

                if (ret == 0 && GetLength(pkiMsg, &localIdx, &length, totalSz) < 0)
                    ret = ASN_PARSE_E;

                if (ret == 0) {
                    /* Use single OCTET_STRING directly. */
                    if (localIdx - start + length == (word32)contentLen)
                        multiPart = 0;
                    localIdx = start;
                }
            }

            /* get length of content in case of single part */
            if (ret == 0 && !multiPart) {
                if (pkiMsg[localIdx++] != ASN_OCTET_STRING)
                    ret = ASN_PARSE_E;

                if (ret == 0 && GetLength(pkiMsg, &localIdx, &length, totalSz) < 0)
                    ret = ASN_PARSE_E;
            }

            /* update idx if successful */
            if (ret == 0) {
                /* support using header and footer without content */
                if (pkiMsg2 && pkiMsg2Sz > 0 && hashBuf && hashSz > 0) {
                    localIdx = 0;
                }
                idx = localIdx;
            }

        #ifndef NO_PKCS7_STREAM
            /* save contentType */
            pkcs7->stream->nonce = (byte*)XMALLOC(contentTypeSz, pkcs7->heap,
                    DYNAMIC_TYPE_PKCS7);
            if (pkcs7->stream->nonce == NULL) {
                ret = MEMORY_E;
                break;
            }
            else {
                pkcs7->stream->nonceSz = contentTypeSz;
                XMEMCPY(pkcs7->stream->nonce, contentType, contentTypeSz);
            }

            if ((ret = wc_PKCS7_StreamEndCase(pkcs7, &stateIdx, &idx)) != 0) {
                break;
            }
            wc_PKCS7_StreamStoreVar(pkcs7, pkiMsg2Sz, localIdx, length);

            /* content expected? */
            if ((ret == 0 && length > 0) &&
                !(pkiMsg2 && pkiMsg2Sz > 0 && hashBuf && hashSz > 0)) {
                pkcs7->stream->expected = length;
            }
            else {
                pkcs7->stream->expected = 0;
            }

            /* content length is in multiple parts */
            if (multiPart) {
                pkcs7->stream->expected = contentLen;
            }
            pkcs7->stream->multi = multiPart;

        #endif
            wc_PKCS7_ChangeState(pkcs7, WC_PKCS7_VERIFY_STAGE3);
            FALL_THROUGH;

        case WC_PKCS7_VERIFY_STAGE3:
        #ifndef NO_PKCS7_STREAM
            if ((ret = wc_PKCS7_AddDataToStream(pkcs7, in, inSz + in2Sz, pkcs7->stream->expected,
                            &pkiMsg, &idx)) != 0) {
                break;
            }

            pkiMsgSz = wc_PKCS7_GetMaxStream(pkcs7, PKCS7_DEFAULT_PEEK, in, inSz);
            wc_PKCS7_StreamGetVar(pkcs7, &pkiMsg2Sz, (int*)&localIdx, &length);

            if (pkcs7->stream->length > 0) {
                localIdx = 0;
            }
            multiPart = pkcs7->stream->multi;
        #endif

            /* Break out before content because it can be optional in degenerate
             * cases. */
            if (ret != 0)
                break;

            /* get parts of content */
            if (ret == 0 && multiPart) {
                int i = 0;
                keepContent = !(pkiMsg2 && pkiMsg2Sz > 0 && hashBuf && hashSz > 0);

                if (keepContent) {
                    /* Create a buffer to hold content of OCTET_STRINGs. */
                    pkcs7->contentDynamic = (byte*)XMALLOC(contentLen, pkcs7->heap,
                                                            DYNAMIC_TYPE_PKCS7);
                    if (pkcs7->contentDynamic == NULL)
                        ret = MEMORY_E;
                }

                start = localIdx;
                /* Use the data from each OCTET_STRING. */
                while (ret == 0 && localIdx < start + contentLen) {
                    if (pkiMsg[localIdx++] != ASN_OCTET_STRING)
                        ret = ASN_PARSE_E;

                    if (ret == 0 && GetLength(pkiMsg, &localIdx, &length, totalSz) < 0)
                        ret = ASN_PARSE_E;
                    if (ret == 0 && length + localIdx > start + contentLen)
                        ret = ASN_PARSE_E;

                    if (ret == 0) {
                        if (keepContent) {
                            XMEMCPY(pkcs7->contentDynamic + i, pkiMsg + localIdx,
                                                                        length);
                        }
                        i += length;
                        localIdx += length;
                    }
                }
                localIdx = start; /* reset for sanity check, increment later */
                length = i;
            }

            /* Save the inner data as the content. */
            if (ret == 0 && length > 0) {
                contentSz = length;

                /* support using header and footer without content */
                if (pkiMsg2 && pkiMsg2Sz > 0 && hashBuf && hashSz > 0) {
                    /* Content not provided, use provided pkiMsg2 footer */
                    content = NULL;
                    localIdx = 0;
                    if (contentSz != (int)pkcs7->contentSz) {
                        WOLFSSL_MSG("Data signed does not match contentSz provided");
                        ret = BUFFER_E;
                    }
                }
                else {
                    if ((word32)length > pkiMsgSz - localIdx) {
                        ret = BUFFER_E;
                    }

                    /* Content pointer for calculating hashes later */
                    if (ret == 0 && !multiPart) {
                        content = &pkiMsg[localIdx];
                    }
                    if (ret == 0 && multiPart) {
                        content = pkcs7->contentDynamic;
                    }

                    if (ret == 0) {
                        idx += length;

                        pkiMsg2   = pkiMsg;
                        pkiMsg2Sz = pkiMsgSz;
                    #ifndef NO_PKCS7_STREAM
                        pkcs7->stream->flagOne = 1;
                    #endif
                    }
                }
            }
            else {
                pkiMsg2 = pkiMsg;
            #ifndef NO_PKCS7_STREAM
                pkcs7->stream->flagOne = 1;
            #endif
            }

            /* If getting the content info failed with non degenerate then return the
             * error case. Otherwise with a degenerate it is ok if the content
             * info was omitted */
            if (!degenerate && ret != 0) {
                break;
            }
            else {
                ret = 0; /* reset ret state on degenerate case */
            }

            /* Get the implicit[0] set of certificates */
            if (pkiMsg2[idx] == (ASN_CONSTRUCTED | ASN_CONTEXT_SPECIFIC | 0)) {
                idx++;
                if (GetLength(pkiMsg2, &idx, &length, pkiMsg2Sz) < 0)
                    ret = ASN_PARSE_E;

            if (ret != 0) {
                break;
            }
        #ifndef NO_PKCS7_STREAM
            /* save content */
            if (content != NULL) {
                XFREE(pkcs7->stream->content, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
                pkcs7->stream->content = (byte*)XMALLOC(contentSz, pkcs7->heap,
                        DYNAMIC_TYPE_PKCS7);
                if (pkcs7->stream->content == NULL) {
                    ret = MEMORY_E;
                    break;
                }
                else {
                    XMEMCPY(pkcs7->stream->content, content, contentSz);
                    pkcs7->stream->contentSz = contentSz;
                }
            }

            if (pkcs7->stream->flagOne) {
                stateIdx = idx; /* case where all data was read from in2 */
            }

            if ((ret = wc_PKCS7_StreamEndCase(pkcs7, &stateIdx, &idx)) != 0) {
                break;
            }
            wc_PKCS7_StreamStoreVar(pkcs7, pkiMsg2Sz, 0, length);
            if (length > 0) {
                pkcs7->stream->expected = length;
            }
            else {
                pkcs7->stream->expected = MAX_SEQ_SZ;
            }
        #endif
            wc_PKCS7_ChangeState(pkcs7, WC_PKCS7_VERIFY_STAGE4);
            FALL_THROUGH;

        case WC_PKCS7_VERIFY_STAGE4:
        #ifndef NO_PKCS7_STREAM
            if ((ret = wc_PKCS7_AddDataToStream(pkcs7, in, inSz + in2Sz,
                            pkcs7->stream->expected, &pkiMsg, &idx)) != 0) {
                break;
            }

            wc_PKCS7_StreamGetVar(pkcs7, &pkiMsg2Sz, 0, &length);
            if (pkcs7->stream->flagOne) {
                pkiMsg2 = pkiMsg;
            }

            /* restore content */
            content   = pkcs7->stream->content;
            contentSz = pkcs7->stream->contentSz;

            /* store certificate if needed */
            if (length > 0 && in2Sz == 0) {
                /* free tmpCert if not NULL */
                XFREE(pkcs7->stream->tmpCert, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
                pkcs7->stream->tmpCert = (byte*)XMALLOC(length,
                        pkcs7->heap, DYNAMIC_TYPE_PKCS7);
                if (pkcs7->stream->tmpCert == NULL) {
                    ret = MEMORY_E;
                    break;
                }
                XMEMCPY(pkcs7->stream->tmpCert, pkiMsg2 + idx, length);
                pkiMsg2 = pkcs7->stream->tmpCert;
                idx = 0;
            }
        #endif

                if (length > 0) {
                    /* At this point, idx is at the first certificate in
                     * a set of certificates. There may be more than one,
                     * or none, or they may be a PKCS 6 extended
                     * certificate. We want to save the first cert if it
                     * is X.509. */

                    word32 certIdx = idx;

                    if (pkiMsg2[certIdx++] == (ASN_CONSTRUCTED | ASN_SEQUENCE)) {
                        if (GetLength(pkiMsg2, &certIdx, &certSz, pkiMsg2Sz) < 0)
                            ret = ASN_PARSE_E;

                        cert = &pkiMsg2[idx];
                        certSz += (certIdx - idx);
                    }
        #ifdef ASN_BER_TO_DER
                    der = pkcs7->der;
        #endif
                    contentDynamic = pkcs7->contentDynamic;

                    if (ret == 0) {
                    #ifndef NO_PKCS7_STREAM
                        PKCS7State* stream = pkcs7->stream;
                    #endif
                        /* This will reset PKCS7 structure and then set the
                         * certificate */
                        ret = wc_PKCS7_InitWithCert(pkcs7, cert, certSz);
                    #ifndef NO_PKCS7_STREAM
                        pkcs7->stream = stream;
                    #endif
                    }
                    pkcs7->contentDynamic = contentDynamic;
        #ifdef ASN_BER_TO_DER
                    pkcs7->der = der;
        #endif
                    if (ret != 0)
                        break;

                    /* iterate through any additional certificates */
                    if (ret == 0 && MAX_PKCS7_CERTS > 0) {
                        int sz = 0;
                        int i;

                        pkcs7->cert[0]   = cert;
                        pkcs7->certSz[0] = certSz;
                        certIdx = idx + certSz;

                        for (i = 1; i < MAX_PKCS7_CERTS &&
                                certIdx + 1 < pkiMsg2Sz &&
                                certIdx + 1 < (word32)length; i++) {
                            localIdx = certIdx;

                            if (pkiMsg2[certIdx++] ==
                                             (ASN_CONSTRUCTED | ASN_SEQUENCE)) {
                                if (GetLength(pkiMsg2, &certIdx, &sz,
                                            pkiMsg2Sz) < 0) {
                                    ret = ASN_PARSE_E;
                                    break;
                                }

                                pkcs7->cert[i]   = &pkiMsg2[localIdx];
                                pkcs7->certSz[i] = sz + (certIdx - localIdx);
                                certIdx += sz;
                            }
                        }
                    }
                }
                idx += length;
            }

            /* set content and size after init of PKCS7 structure */
            pkcs7->content   = content;
            pkcs7->contentSz = contentSz;

            if (ret != 0) {
                break;
            }
        #ifndef NO_PKCS7_STREAM
            /* factor in that recent idx was in cert buffer. If in2 buffer was
             * used then don't advance idx. */
            if (pkcs7->stream->flagOne && pkcs7->stream->length == 0) {
                idx = stateIdx + idx;
            }
            else {
                stateIdx = idx; /* didn't read any from internal buffer */
            }

            if ((ret = wc_PKCS7_StreamEndCase(pkcs7, &stateIdx, &idx)) != 0) {
                break;
            }
            if (pkcs7->stream->flagOne && pkcs7->stream->length > 0) {
                idx = stateIdx + idx;
            }

            pkcs7->stream->expected = MAX_OID_SZ + ASN_TAG_SZ + MAX_LENGTH_SZ +
                                      MAX_SET_SZ;

            if (pkcs7->stream->expected > (pkcs7->stream->maxLen -
                                pkcs7->stream->totalRd) + pkcs7->stream->length)
                pkcs7->stream->expected = (pkcs7->stream->maxLen -
                                pkcs7->stream->totalRd) + pkcs7->stream->length;

            wc_PKCS7_StreamStoreVar(pkcs7, pkiMsg2Sz, 0, length);
        #endif
            wc_PKCS7_ChangeState(pkcs7, WC_PKCS7_VERIFY_STAGE5);
            FALL_THROUGH;

        case WC_PKCS7_VERIFY_STAGE5:
        #ifndef NO_PKCS7_STREAM
            if ((ret = wc_PKCS7_AddDataToStream(pkcs7, in, inSz,
                            pkcs7->stream->expected, &pkiMsg, &idx)) != 0) {
                break;
            }
            wc_PKCS7_StreamGetVar(pkcs7, &pkiMsg2Sz, 0, &length);
            if (pkcs7->stream->flagOne) {
                pkiMsg2 = pkiMsg;
            }

            /* restore content type */
            contentType   = pkcs7->stream->nonce;
            contentTypeSz = pkcs7->stream->nonceSz;
        #endif

            /* set contentType and size after init of PKCS7 structure */
            if (ret == 0 && wc_PKCS7_SetContentType(pkcs7, contentType,
                        contentTypeSz) < 0)
                ret = ASN_PARSE_E;

            /* Get the implicit[1] set of crls */
            if (ret == 0 && idx > pkiMsg2Sz)
                ret = BUFFER_E;

            if (ret == 0 && pkiMsg2[idx] ==
                                 (ASN_CONSTRUCTED | ASN_CONTEXT_SPECIFIC | 1)) {
                idx++;
                if (GetLength(pkiMsg2, &idx, &length, pkiMsg2Sz) < 0)
                    ret = ASN_PARSE_E;

                /* Skip the set */
                idx += length;
            }

            /* Get the set of signerInfos */
            if (ret == 0 && GetSet(pkiMsg2, &idx, &length, pkiMsg2Sz) < 0)
                ret = ASN_PARSE_E;

            if (ret != 0)
                break;
        #ifndef NO_PKCS7_STREAM
            if (!pkcs7->stream->flagOne) {
                stateIdx = idx; /* didn't read any from internal buffer */
            }
            if ((ret = wc_PKCS7_StreamEndCase(pkcs7, &stateIdx, &idx)) != 0) {
                break;
            }
            wc_PKCS7_StreamStoreVar(pkcs7, pkiMsg2Sz, 0, length);

            if (length > 0) {
                pkcs7->stream->expected = length;
            }
            else {
                pkcs7->stream->expected = 0;
            }
        #endif
            wc_PKCS7_ChangeState(pkcs7, WC_PKCS7_VERIFY_STAGE6);
            FALL_THROUGH;

        case WC_PKCS7_VERIFY_STAGE6:
        #ifndef NO_PKCS7_STREAM
            if ((ret = wc_PKCS7_AddDataToStream(pkcs7, in, inSz + in2Sz,
                            pkcs7->stream->expected, &pkiMsg, &idx)) != 0) {
                break;
            }

            wc_PKCS7_StreamGetVar(pkcs7, &pkiMsg2Sz, 0, &length);
            if (pkcs7->stream->flagOne) {
                pkiMsg2 = pkiMsg;
            }

            /* restore content */
            content   = pkcs7->stream->content;
            contentSz = pkcs7->stream->contentSz;
        #endif

            /* require a signer if degenerate case not allowed */
            if (ret == 0 && length == 0 && pkcs7->noDegenerate == 1)
                ret = PKCS7_NO_SIGNER_E;

            if (ret == 0 && degenerate == 0 && length == 0) {
                WOLFSSL_MSG("PKCS7 signers expected");
                ret = PKCS7_NO_SIGNER_E;
            }

            if (ret == 0 && length > 0 && degenerate == 0) {
                /* Get the sequence of the first signerInfo */
                if (ret == 0 && GetSequence(pkiMsg2, &idx, &length,
                            pkiMsg2Sz) < 0)
                    ret = ASN_PARSE_E;

                /* Get the version */
                if (ret == 0 && GetMyVersion(pkiMsg2, &idx, &version,
                            pkiMsg2Sz) < 0)
                    ret = ASN_PARSE_E;

                if (ret == 0 && version == 1) {
                    /* Get the sequence of IssuerAndSerialNumber */
                    if (GetSequence(pkiMsg2, &idx, &length, pkiMsg2Sz) < 0)
                        ret = ASN_PARSE_E;

                    /* Skip it */
                    idx += length;

                } else if (ret == 0 && version == 3) {
                    /* Get the sequence of SubjectKeyIdentifier */
                    if (pkiMsg2[idx++] != (ASN_CONSTRUCTED |
                                               ASN_CONTEXT_SPECIFIC | 0)) {
                        ret = ASN_PARSE_E;
                    }

                    if (ret == 0 && GetLength(pkiMsg2, &idx, &length,
                                              pkiMsg2Sz) <= 0) {
                        ret = ASN_PARSE_E;
                    }

                    if (ret == 0 && pkiMsg2[idx++] != ASN_OCTET_STRING)
                        ret = ASN_PARSE_E;

                    if (ret == 0 && GetLength(pkiMsg2, &idx, &length, pkiMsg2Sz) < 0)
                        ret = ASN_PARSE_E;

                    /* Skip it */
                    idx += length;

                } else {
                    WOLFSSL_MSG("PKCS#7 signerInfo version must be 1 or 3");
                    ret = ASN_VERSION_E;
                }

                /* Get the sequence of digestAlgorithm */
                if (ret == 0 && GetAlgoId(pkiMsg2, &idx, &hashOID, oidHashType,
                            pkiMsg2Sz) < 0) {
                    ret = ASN_PARSE_E;
                }
                pkcs7->hashOID = (int)hashOID;

                /* Get the IMPLICIT[0] SET OF signedAttributes */
                if (ret == 0 && pkiMsg2[idx] ==
                        (ASN_CONSTRUCTED | ASN_CONTEXT_SPECIFIC | 0)) {
                    idx++;

                    if (GetLength(pkiMsg2, &idx, &length, pkiMsg2Sz) < 0)
                        ret = ASN_PARSE_E;

                    /* save pointer and length */
                    signedAttrib = &pkiMsg2[idx];
                    signedAttribSz = length;

                    if (ret == 0 && wc_PKCS7_ParseAttribs(pkcs7, signedAttrib,
                                signedAttribSz) < 0) {
                        WOLFSSL_MSG("Error parsing signed attributes");
                        ret = ASN_PARSE_E;
                    }

                    idx += length;
                }

                /* Get digestEncryptionAlgorithm */
                if (ret == 0 && GetAlgoId(pkiMsg2, &idx, &sigOID, oidSigType,
                            pkiMsg2Sz) < 0) {
                    ret = ASN_PARSE_E;
                    break;
                }

                /* store public key type based on digestEncryptionAlgorithm */
                if ((ret = 0) && ((ret = wc_PKCS7_SetPublicKeyOID(pkcs7, sigOID))
                        <= 0)) {
                    WOLFSSL_MSG("Failed to set public key OID from signature");
                }

                /* Get the signature */
                if (ret == 0 && pkiMsg2[idx] == ASN_OCTET_STRING) {
                    idx++;

                    if (GetLength(pkiMsg2, &idx, &length, pkiMsg2Sz) < 0)
                        ret = ASN_PARSE_E;

                    /* save pointer and length */
                    sig = &pkiMsg2[idx];
                    sigSz = length;

                    idx += length;
                }

                pkcs7->content = content;
                pkcs7->contentSz = contentSz;

                if (ret == 0) {
                    ret = wc_PKCS7_SignedDataVerifySignature(pkcs7, sig, sigSz,
                                                   signedAttrib, signedAttribSz,
                                                   hashBuf, hashSz);
                }
            }

            if (ret < 0)
                break;

            ret = 0; /* success */
        #ifndef NO_PKCS7_STREAM
            wc_PKCS7_ResetStream(pkcs7);
        #endif
            wc_PKCS7_ChangeState(pkcs7, WC_PKCS7_START);
            break;

        default:
            WOLFSSL_MSG("PKCS7 Unknown verify state");
            ret = BAD_FUNC_ARG;
    }

    if (ret != 0 && ret != WC_PKCS7_WANT_READ_E) {
    #ifndef NO_PKCS7_STREAM
        wc_PKCS7_ResetStream(pkcs7);
    #endif
        wc_PKCS7_ChangeState(pkcs7, WC_PKCS7_START);
    }
    return ret;
}


/* variant that allows computed data hash and header/foot,
 * which is useful for large data signing */
int wc_PKCS7_VerifySignedData_ex(PKCS7* pkcs7, const byte* hashBuf,
    word32 hashSz, byte* pkiMsgHead, word32 pkiMsgHeadSz, byte* pkiMsgFoot,
    word32 pkiMsgFootSz)
{
    return PKCS7_VerifySignedData(pkcs7, hashBuf, hashSz,
        pkiMsgHead, pkiMsgHeadSz, pkiMsgFoot, pkiMsgFootSz);
}

int wc_PKCS7_VerifySignedData(PKCS7* pkcs7, byte* pkiMsg, word32 pkiMsgSz)
{
    return PKCS7_VerifySignedData(pkcs7, NULL, 0, pkiMsg, pkiMsgSz, NULL, 0);
}


/* Generate random content encryption key, store into pkcs7->cek and
 * pkcs7->cekSz.
 *
 * pkcs7 - pointer to initialized PKCS7 structure
 * len   - length of key to be generated
 *
 * Returns 0 on success, negative upon error */
static int PKCS7_GenerateContentEncryptionKey(PKCS7* pkcs7, word32 len)
{
    int ret;
    WC_RNG rng;
    byte* tmpKey;

    if (pkcs7 == NULL || len == 0)
        return BAD_FUNC_ARG;

    /* if key already exists, don't need to re-generate */
    if (pkcs7->cek != NULL && pkcs7->cekSz != 0) {

        /* if key exists, but is different size, return error */
        if (pkcs7->cekSz != len) {
            WOLFSSL_MSG("Random content-encryption key size is inconsistent "
                        "between CMS recipients");
            return WC_KEY_SIZE_E;
        }

        return 0;
    }

    /* allocate space for cek */
    tmpKey = (byte*)XMALLOC(len, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
    if (tmpKey == NULL)
        return MEMORY_E;

    XMEMSET(tmpKey, 0, len);

    ret = wc_InitRng_ex(&rng, pkcs7->heap, pkcs7->devId);
    if (ret != 0) {
        XFREE(tmpKey, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        return ret;
    }

    ret = wc_RNG_GenerateBlock(&rng, tmpKey, len);
    if (ret != 0) {
        wc_FreeRng(&rng);
        XFREE(tmpKey, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        return ret;
    }

    /* store into PKCS7, memory freed during final cleanup */
    pkcs7->cek = tmpKey;
    pkcs7->cekSz = len;

    wc_FreeRng(&rng);

    return 0;
}


/* wrap CEK (content encryption key) with KEK, 0 on success, < 0 on error */
static int wc_PKCS7_KeyWrap(byte* cek, word32 cekSz, byte* kek,
                            word32 kekSz, byte* out, word32 outSz,
                            int keyWrapAlgo, int direction)
{
    int ret;

    if (cek == NULL || kek == NULL || out == NULL)
        return BAD_FUNC_ARG;

    switch (keyWrapAlgo) {
#ifndef NO_AES
    #ifdef WOLFSSL_AES_128
        case AES128_WRAP:
    #endif
    #ifdef WOLFSSL_AES_192
        case AES192_WRAP:
    #endif
    #ifdef WOLFSSL_AES_256
        case AES256_WRAP:
    #endif

            if (direction == AES_ENCRYPTION) {

                ret = wc_AesKeyWrap(kek, kekSz, cek, cekSz,
                                    out, outSz, NULL);

            } else if (direction == AES_DECRYPTION) {

                ret = wc_AesKeyUnWrap(kek, kekSz, cek, cekSz,
                                      out, outSz, NULL);
            } else {
                WOLFSSL_MSG("Bad key un/wrap direction");
                return BAD_FUNC_ARG;
            }

            if (ret <= 0)
                return ret;
            break;
#endif /* NO_AES */

        default:
            WOLFSSL_MSG("Unsupported key wrap algorithm");
            return BAD_KEYWRAP_ALG_E;
    };

    (void)cekSz;
    (void)kekSz;
    (void)outSz;
    (void)direction;
    return ret;
}


#ifdef HAVE_ECC

/* KARI == KeyAgreeRecipientInfo (key agreement) */
typedef struct WC_PKCS7_KARI {
    DecodedCert* decoded;          /* decoded recip cert */
    void*    heap;                 /* user heap, points to PKCS7->heap */
    int      devId;                /* device ID for HW based private key */
    ecc_key* recipKey;             /* recip key  (pub | priv) */
    ecc_key* senderKey;            /* sender key (pub | priv) */
    byte*    senderKeyExport;      /* sender ephemeral key DER */
    byte*    kek;                  /* key encryption key */
    byte*    ukm;                  /* OPTIONAL user keying material */
    byte*    sharedInfo;           /* ECC-CMS-SharedInfo ASN.1 encoded blob */
    word32   senderKeyExportSz;    /* size of sender ephemeral key DER */
    word32   kekSz;                /* size of key encryption key */
    word32   ukmSz;                /* size of user keying material */
    word32   sharedInfoSz;         /* size of ECC-CMS-SharedInfo encoded */
    byte     ukmOwner;             /* do we own ukm buffer? 1:yes, 0:no */
    byte     direction;            /* WC_PKCS7_ENCODE | WC_PKCS7_DECODE */
    byte     decodedInit : 1;      /* indicates decoded was initialized */
    byte     recipKeyInit : 1;     /* indicates recipKey was initialized */
    byte     senderKeyInit : 1;    /* indicates senderKey was initialized */
} WC_PKCS7_KARI;


/* allocate and create new WC_PKCS7_KARI struct,
 * returns struct pointer on success, NULL on failure */
static WC_PKCS7_KARI* wc_PKCS7_KariNew(PKCS7* pkcs7, byte direction)
{
    WC_PKCS7_KARI* kari = NULL;

    if (pkcs7 == NULL)
        return NULL;

    kari = (WC_PKCS7_KARI*)XMALLOC(sizeof(WC_PKCS7_KARI), pkcs7->heap,
                                   DYNAMIC_TYPE_PKCS7);
    if (kari == NULL) {
        WOLFSSL_MSG("Failed to allocate WC_PKCS7_KARI");
        return NULL;
    }

    kari->decoded = (DecodedCert*)XMALLOC(sizeof(DecodedCert), pkcs7->heap,
                                          DYNAMIC_TYPE_PKCS7);
    if (kari->decoded == NULL) {
        WOLFSSL_MSG("Failed to allocate DecodedCert");
        XFREE(kari, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        return NULL;
    }

    kari->recipKey = (ecc_key*)XMALLOC(sizeof(ecc_key), pkcs7->heap,
                                       DYNAMIC_TYPE_PKCS7);
    if (kari->recipKey == NULL) {
        WOLFSSL_MSG("Failed to allocate recipient ecc_key");
        XFREE(kari->decoded, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        XFREE(kari, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        return NULL;
    }

    kari->senderKey = (ecc_key*)XMALLOC(sizeof(ecc_key), pkcs7->heap,
                                        DYNAMIC_TYPE_PKCS7);
    if (kari->senderKey == NULL) {
        WOLFSSL_MSG("Failed to allocate sender ecc_key");
        XFREE(kari->recipKey, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        XFREE(kari->decoded, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        XFREE(kari, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        return NULL;
    }

    kari->senderKeyExport = NULL;
    kari->senderKeyExportSz = 0;
    kari->kek = NULL;
    kari->kekSz = 0;
    kari->ukm = NULL;
    kari->ukmSz = 0;
    kari->ukmOwner = 0;
    kari->sharedInfo = NULL;
    kari->sharedInfoSz = 0;
    kari->direction = direction;
    kari->decodedInit = 0;
    kari->recipKeyInit = 0;
    kari->senderKeyInit = 0;

    kari->heap = pkcs7->heap;
    kari->devId = pkcs7->devId;

    return kari;
}


/* free WC_PKCS7_KARI struct, return 0 on success */
static int wc_PKCS7_KariFree(WC_PKCS7_KARI* kari)
{
    void* heap;

    if (kari) {
        heap = kari->heap;

        if (kari->decoded) {
            if (kari->decodedInit)
                FreeDecodedCert(kari->decoded);
            XFREE(kari->decoded, heap, DYNAMIC_TYPE_PKCS7);
        }
        if (kari->senderKey) {
            if (kari->senderKeyInit)
                wc_ecc_free(kari->senderKey);
            XFREE(kari->senderKey, heap, DYNAMIC_TYPE_PKCS7);
        }
        if (kari->recipKey) {
            if (kari->recipKeyInit)
                wc_ecc_free(kari->recipKey);
            XFREE(kari->recipKey, heap, DYNAMIC_TYPE_PKCS7);
        }
        if (kari->senderKeyExport) {
            ForceZero(kari->senderKeyExport, kari->senderKeyExportSz);
            XFREE(kari->senderKeyExport, heap, DYNAMIC_TYPE_PKCS7);
            kari->senderKeyExportSz = 0;
        }
        if (kari->kek) {
            ForceZero(kari->kek, kari->kekSz);
            XFREE(kari->kek, heap, DYNAMIC_TYPE_PKCS7);
            kari->kekSz = 0;
        }
        if (kari->ukm) {
            if (kari->ukmOwner == 1) {
                XFREE(kari->ukm, heap, DYNAMIC_TYPE_PKCS7);
            }
            kari->ukmSz = 0;
        }
        if (kari->sharedInfo) {
            ForceZero(kari->sharedInfo, kari->sharedInfoSz);
            XFREE(kari->sharedInfo, heap, DYNAMIC_TYPE_PKCS7);
            kari->sharedInfoSz = 0;
        }
        XFREE(kari, heap, DYNAMIC_TYPE_PKCS7);
    }

    (void)heap;

    return 0;
}


/* parse recipient cert/key, return 0 on success, negative on error
 * key/keySz only needed during decoding (WC_PKCS7_DECODE) */
static int wc_PKCS7_KariParseRecipCert(WC_PKCS7_KARI* kari, const byte* cert,
                                       word32 certSz, const byte* key,
                                       word32 keySz)
{
    int ret;
    word32 idx;

    if (kari == NULL || kari->decoded == NULL ||
        cert == NULL || certSz == 0)
        return BAD_FUNC_ARG;

    /* decode certificate */
    InitDecodedCert(kari->decoded, (byte*)cert, certSz, kari->heap);
    kari->decodedInit = 1;
    ret = ParseCert(kari->decoded, CA_TYPE, NO_VERIFY, 0);
    if (ret < 0)
        return ret;

    /* only supports ECDSA for now */
    if (kari->decoded->keyOID != ECDSAk) {
        WOLFSSL_MSG("CMS KARI only supports ECDSA key types");
        return BAD_FUNC_ARG;
    }

    /* make sure subject key id was read from cert */
    if (kari->decoded->extSubjKeyIdSet == 0) {
        WOLFSSL_MSG("Failed to read subject key ID from recipient cert");
        return BAD_FUNC_ARG;
    }

    ret = wc_ecc_init_ex(kari->recipKey, kari->heap, kari->devId);
    if (ret != 0)
        return ret;

    kari->recipKeyInit = 1;

    /* get recip public key */
    if (kari->direction == WC_PKCS7_ENCODE) {

        idx = 0;
        ret = wc_EccPublicKeyDecode(kari->decoded->publicKey, &idx,
                                    kari->recipKey, kari->decoded->pubKeySize);
        if (ret != 0)
            return ret;
    }
    /* get recip private key */
    else if (kari->direction == WC_PKCS7_DECODE) {
        if (key != NULL && keySz > 0) {
            idx = 0;
            ret = wc_EccPrivateKeyDecode(key, &idx, kari->recipKey, keySz);
        }
        else if (kari->devId == INVALID_DEVID) {
            ret = BAD_FUNC_ARG;
        }
        if (ret != 0)
            return ret;

    } else {
        /* bad direction */
        return BAD_FUNC_ARG;
    }

    (void)idx;

    return 0;
}


/* create ephemeral ECC key, places ecc_key in kari->senderKey,
 * DER encoded in kari->senderKeyExport. return 0 on success,
 * negative on error */
static int wc_PKCS7_KariGenerateEphemeralKey(WC_PKCS7_KARI* kari)
{
    int ret;
    WC_RNG rng;

    if (kari == NULL || kari->decoded == NULL ||
        kari->recipKey == NULL || kari->recipKey->dp == NULL)
        return BAD_FUNC_ARG;

    kari->senderKeyExport = (byte*)XMALLOC(kari->decoded->pubKeySize,
                                           kari->heap, DYNAMIC_TYPE_PKCS7);
    if (kari->senderKeyExport == NULL)
        return MEMORY_E;

    kari->senderKeyExportSz = kari->decoded->pubKeySize;

    ret = wc_ecc_init_ex(kari->senderKey, kari->heap, kari->devId);
    if (ret != 0) {
        XFREE(kari->senderKeyExport, kari->heap, DYNAMIC_TYPE_PKCS7);
        return ret;
    }

    kari->senderKeyInit = 1;

    ret = wc_InitRng_ex(&rng, kari->heap, kari->devId);
    if (ret != 0) {
        XFREE(kari->senderKeyExport, kari->heap, DYNAMIC_TYPE_PKCS7);
        return ret;
    }

    ret = wc_ecc_make_key_ex(&rng, kari->recipKey->dp->size,
                             kari->senderKey, kari->recipKey->dp->id);
    if (ret != 0) {
        XFREE(kari->senderKeyExport, kari->heap, DYNAMIC_TYPE_PKCS7);
        wc_FreeRng(&rng);
        return ret;
    }

    wc_FreeRng(&rng);

    /* dump generated key to X.963 DER for output in CMS bundle */
    ret = wc_ecc_export_x963(kari->senderKey, kari->senderKeyExport,
                             &kari->senderKeyExportSz);
    if (ret != 0) {
        XFREE(kari->senderKeyExport, kari->heap, DYNAMIC_TYPE_PKCS7);
        return ret;
    }

    return 0;
}


/* create ASN.1 encoded ECC-CMS-SharedInfo using specified key wrap algorithm,
 * place in kari->sharedInfo. returns 0 on success, negative on error */
static int wc_PKCS7_KariGenerateSharedInfo(WC_PKCS7_KARI* kari, int keyWrapOID)
{
    int idx = 0;
    int sharedInfoSeqSz = 0;
    int keyInfoSz = 0;
    int suppPubInfoSeqSz = 0;
    int entityUInfoOctetSz = 0;
    int entityUInfoExplicitSz = 0;
    int kekOctetSz = 0;
    int sharedInfoSz = 0;

    word32 kekBitSz = 0;

    byte sharedInfoSeq[MAX_SEQ_SZ];
    byte keyInfo[MAX_ALGO_SZ];
    byte suppPubInfoSeq[MAX_SEQ_SZ];
    byte entityUInfoOctet[MAX_OCTET_STR_SZ];
    byte entityUInfoExplicitSeq[MAX_SEQ_SZ];
    byte kekOctet[MAX_OCTET_STR_SZ];

    if (kari == NULL)
        return BAD_FUNC_ARG;

    if ((kari->ukmSz > 0) && (kari->ukm == NULL))
        return BAD_FUNC_ARG;

    /* kekOctet */
    kekOctetSz = SetOctetString(sizeof(word32), kekOctet);
    sharedInfoSz += (kekOctetSz + sizeof(word32));

    /* suppPubInfo */
    suppPubInfoSeqSz = SetImplicit(ASN_SEQUENCE, 2,
                                   kekOctetSz + sizeof(word32),
                                   suppPubInfoSeq);
    sharedInfoSz += suppPubInfoSeqSz;

    /* optional ukm/entityInfo */
    if (kari->ukmSz > 0) {
        entityUInfoOctetSz = SetOctetString(kari->ukmSz, entityUInfoOctet);
        sharedInfoSz += (entityUInfoOctetSz + kari->ukmSz);

        entityUInfoExplicitSz = SetExplicit(0, entityUInfoOctetSz +
                                            kari->ukmSz,
                                            entityUInfoExplicitSeq);
        sharedInfoSz += entityUInfoExplicitSz;
    }

    /* keyInfo */
    keyInfoSz = SetAlgoID(keyWrapOID, keyInfo, oidKeyWrapType, 0);
    sharedInfoSz += keyInfoSz;

    /* sharedInfo */
    sharedInfoSeqSz = SetSequence(sharedInfoSz, sharedInfoSeq);
    sharedInfoSz += sharedInfoSeqSz;

    kari->sharedInfo = (byte*)XMALLOC(sharedInfoSz, kari->heap,
                                      DYNAMIC_TYPE_PKCS7);
    if (kari->sharedInfo == NULL)
        return MEMORY_E;

    kari->sharedInfoSz = sharedInfoSz;

    XMEMCPY(kari->sharedInfo + idx, sharedInfoSeq, sharedInfoSeqSz);
    idx += sharedInfoSeqSz;
    XMEMCPY(kari->sharedInfo + idx, keyInfo, keyInfoSz);
    idx += keyInfoSz;
    if (kari->ukmSz > 0) {
        XMEMCPY(kari->sharedInfo + idx, entityUInfoExplicitSeq,
                entityUInfoExplicitSz);
        idx += entityUInfoExplicitSz;
        XMEMCPY(kari->sharedInfo + idx, entityUInfoOctet, entityUInfoOctetSz);
        idx += entityUInfoOctetSz;
        XMEMCPY(kari->sharedInfo + idx, kari->ukm, kari->ukmSz);
        idx += kari->ukmSz;
    }
    XMEMCPY(kari->sharedInfo + idx, suppPubInfoSeq, suppPubInfoSeqSz);
    idx += suppPubInfoSeqSz;
    XMEMCPY(kari->sharedInfo + idx, kekOctet, kekOctetSz);
    idx += kekOctetSz;

    kekBitSz = (kari->kekSz) * 8;              /* convert to bits */
#ifdef LITTLE_ENDIAN_ORDER
    kekBitSz = ByteReverseWord32(kekBitSz);    /* network byte order */
#endif
    XMEMCPY(kari->sharedInfo + idx, &kekBitSz, sizeof(kekBitSz));

    return 0;
}


/* create key encryption key (KEK) using key wrap algorithm and key encryption
 * algorithm, place in kari->kek. return 0 on success, <0 on error. */
static int wc_PKCS7_KariGenerateKEK(WC_PKCS7_KARI* kari,
                                    int keyWrapOID, int keyEncOID)
{
    int ret;
    int kSz;
    enum wc_HashType kdfType;
    byte*  secret;
    word32 secretSz;

    if (kari == NULL || kari->recipKey == NULL ||
        kari->senderKey == NULL || kari->senderKey->dp == NULL)
        return BAD_FUNC_ARG;

    /* get KEK size, allocate buff */
    kSz = wc_PKCS7_GetOIDKeySize(keyWrapOID);
    if (kSz < 0)
        return kSz;

    kari->kek = (byte*)XMALLOC(kSz, kari->heap, DYNAMIC_TYPE_PKCS7);
    if (kari->kek == NULL)
        return MEMORY_E;

    kari->kekSz = (word32)kSz;

    /* generate ECC-CMS-SharedInfo */
    ret = wc_PKCS7_KariGenerateSharedInfo(kari, keyWrapOID);
    if (ret != 0)
        return ret;

    /* generate shared secret */
    secretSz = kari->senderKey->dp->size;
    secret = (byte*)XMALLOC(secretSz, kari->heap, DYNAMIC_TYPE_PKCS7);
    if (secret == NULL)
        return MEMORY_E;

    if (kari->direction == WC_PKCS7_ENCODE) {

        ret = wc_ecc_shared_secret(kari->senderKey, kari->recipKey,
                                   secret, &secretSz);

    } else if (kari->direction == WC_PKCS7_DECODE) {

        ret = wc_ecc_shared_secret(kari->recipKey, kari->senderKey,
                                   secret, &secretSz);

    } else {
        /* bad direction */
        XFREE(secret, kari->heap, DYNAMIC_TYPE_PKCS7);
        return BAD_FUNC_ARG;
    }

    if (ret != 0) {
        XFREE(secret, kari->heap, DYNAMIC_TYPE_PKCS7);
        return ret;
    }

    /* run through KDF */
    switch (keyEncOID) {

    #ifndef NO_SHA
        case dhSinglePass_stdDH_sha1kdf_scheme:
            kdfType = WC_HASH_TYPE_SHA;
            break;
    #endif
    #ifndef WOLFSSL_SHA224
        case dhSinglePass_stdDH_sha224kdf_scheme:
            kdfType = WC_HASH_TYPE_SHA224;
            break;
    #endif
    #ifndef NO_SHA256
        case dhSinglePass_stdDH_sha256kdf_scheme:
            kdfType = WC_HASH_TYPE_SHA256;
            break;
    #endif
    #ifdef WOLFSSL_SHA384
        case dhSinglePass_stdDH_sha384kdf_scheme:
            kdfType = WC_HASH_TYPE_SHA384;
            break;
    #endif
    #ifdef WOLFSSL_SHA512
        case dhSinglePass_stdDH_sha512kdf_scheme:
            kdfType = WC_HASH_TYPE_SHA512;
            break;
    #endif
        default:
            WOLFSSL_MSG("Unsupported key agreement algorithm");
            XFREE(secret, kari->heap, DYNAMIC_TYPE_PKCS7);
            return BAD_FUNC_ARG;
    };

    ret = wc_X963_KDF(kdfType, secret, secretSz, kari->sharedInfo,
                      kari->sharedInfoSz, kari->kek, kari->kekSz);
    if (ret != 0) {
        XFREE(secret, kari->heap, DYNAMIC_TYPE_PKCS7);
        return ret;
    }

    XFREE(secret, kari->heap, DYNAMIC_TYPE_PKCS7);

    return 0;
}


/* Encode and add CMS EnvelopedData KARI (KeyAgreeRecipientInfo) RecipientInfo
 * to CMS/PKCS#7 EnvelopedData structure.
 *
 * Returns 0 on success, negative upon error */
int wc_PKCS7_AddRecipient_KARI(PKCS7* pkcs7, const byte* cert, word32 certSz,
                               int keyWrapOID, int keyAgreeOID, byte* ukm,
                               word32 ukmSz, int options)
{
    Pkcs7EncodedRecip* recip = NULL;
    Pkcs7EncodedRecip* lastRecip = NULL;
    WC_PKCS7_KARI* kari = NULL;

    word32 idx = 0;
    word32 encryptedKeySz = MAX_ENCRYPTED_KEY_SZ;

    int ret = 0;
    int keySz, direction = 0;
    int blockKeySz = 0;

    /* ASN.1 layout */
    int totalSz = 0;
    int kariSeqSz = 0;
    byte kariSeq[MAX_SEQ_SZ];           /* IMPLICIT [1] */
    int verSz = 0;
    byte ver[MAX_VERSION_SZ];

    int origIdOrKeySeqSz = 0;
    byte origIdOrKeySeq[MAX_SEQ_SZ];    /* IMPLICIT [0] */
    int origPubKeySeqSz = 0;
    byte origPubKeySeq[MAX_SEQ_SZ];     /* IMPLICIT [1] */
    int origAlgIdSz = 0;
    byte origAlgId[MAX_ALGO_SZ];
    int origPubKeyStrSz = 0;
    byte origPubKeyStr[MAX_OCTET_STR_SZ];

    /* optional user keying material */
    int ukmOctetSz = 0;
    byte ukmOctetStr[MAX_OCTET_STR_SZ];
    int ukmExplicitSz = 0;
    byte ukmExplicitSeq[MAX_SEQ_SZ];

    int keyEncryptAlgoIdSz = 0;
    byte keyEncryptAlgoId[MAX_ALGO_SZ];
    int keyWrapAlgSz = 0;
    byte keyWrapAlg[MAX_ALGO_SZ];

    int recipEncKeysSeqSz = 0;
    byte recipEncKeysSeq[MAX_SEQ_SZ];
    int recipEncKeySeqSz = 0;
    byte recipEncKeySeq[MAX_SEQ_SZ];
    int recipKeyIdSeqSz = 0;
    byte recipKeyIdSeq[MAX_SEQ_SZ];     /* IMPLICIT [0] */
    int subjKeyIdOctetSz = 0;
    byte subjKeyIdOctet[MAX_OCTET_STR_SZ];
    int encryptedKeyOctetSz = 0;
    byte encryptedKeyOctet[MAX_OCTET_STR_SZ];

#ifdef WOLFSSL_SMALL_STACK
    byte* encryptedKey;

    encryptedKey = (byte*)XMALLOC(MAX_ENCRYPTED_KEY_SZ, pkcs7->heap,
                                  DYNAMIC_TYPE_TMP_BUFFER);
    if (encryptedKey == NULL) {
        return MEMORY_E;
    }
#else
    byte encryptedKey[MAX_ENCRYPTED_KEY_SZ];
#endif

    /* allocate and init memory for recipient */
    recip = (Pkcs7EncodedRecip*)XMALLOC(sizeof(Pkcs7EncodedRecip), pkcs7->heap,
                                 DYNAMIC_TYPE_PKCS7);
    if (recip == NULL) {
#ifdef WOLFSSL_SMALL_STACK
        XFREE(encryptedKey, pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
#endif
        return MEMORY_E;
    }
    XMEMSET(recip, 0, sizeof(Pkcs7EncodedRecip));

    /* get key size for content-encryption key based on algorithm */
    blockKeySz = wc_PKCS7_GetOIDKeySize(pkcs7->encryptOID);
    if (blockKeySz < 0) {
#ifdef WOLFSSL_SMALL_STACK
        XFREE(encryptedKey, pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
#endif
        XFREE(recip, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        return blockKeySz;
    }

    /* generate random content encryption key, if needed */
    ret = PKCS7_GenerateContentEncryptionKey(pkcs7, blockKeySz);
    if (ret < 0) {
#ifdef WOLFSSL_SMALL_STACK
        XFREE(encryptedKey, pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
#endif
        XFREE(recip, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        return ret;
    }

    /* set direction based on keyWrapAlgo */
    switch (keyWrapOID) {
#ifndef NO_AES
    #ifdef WOLFSSL_AES_128
        case AES128_WRAP:
    #endif
    #ifdef WOLFSSL_AES_192
        case AES192_WRAP:
    #endif
    #ifdef WOLFSSL_AES_256
        case AES256_WRAP:
    #endif
            direction = AES_ENCRYPTION;
            break;
#endif
        default:
            WOLFSSL_MSG("Unsupported key wrap algorithm");
#ifdef WOLFSSL_SMALL_STACK
            XFREE(encryptedKey, pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
#endif
            XFREE(recip, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
            return BAD_KEYWRAP_ALG_E;
    }

    kari = wc_PKCS7_KariNew(pkcs7, WC_PKCS7_ENCODE);
    if (kari == NULL) {
#ifdef WOLFSSL_SMALL_STACK
        XFREE(encryptedKey, pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
#endif
        XFREE(recip, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        return MEMORY_E;
    }

    /* set user keying material if available */
    if (ukmSz > 0 && ukm != NULL) {
        kari->ukm = ukm;
        kari->ukmSz = ukmSz;
        kari->ukmOwner = 0;
    }

    /* parse recipient cert, get public key */
    ret = wc_PKCS7_KariParseRecipCert(kari, cert, certSz, NULL, 0);
    if (ret != 0) {
        wc_PKCS7_KariFree(kari);
#ifdef WOLFSSL_SMALL_STACK
        XFREE(encryptedKey, pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
#endif
        XFREE(recip, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        return ret;
    }

    /* generate sender ephemeral ECC key */
    ret = wc_PKCS7_KariGenerateEphemeralKey(kari);
    if (ret != 0) {
        wc_PKCS7_KariFree(kari);
#ifdef WOLFSSL_SMALL_STACK
        XFREE(encryptedKey, pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
#endif
        XFREE(recip, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        return ret;
    }

    /* generate KEK (key encryption key) */
    ret = wc_PKCS7_KariGenerateKEK(kari, keyWrapOID, keyAgreeOID);
    if (ret != 0) {
        wc_PKCS7_KariFree(kari);
#ifdef WOLFSSL_SMALL_STACK
        XFREE(encryptedKey, pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
#endif
        XFREE(recip, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        return ret;
    }

    /* encrypt CEK with KEK */
    keySz = wc_PKCS7_KeyWrap(pkcs7->cek, pkcs7->cekSz, kari->kek,
                             kari->kekSz, encryptedKey, encryptedKeySz,
                             keyWrapOID, direction);
    if (keySz <= 0) {
        wc_PKCS7_KariFree(kari);
#ifdef WOLFSSL_SMALL_STACK
        XFREE(encryptedKey, pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
#endif
        XFREE(recip, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        return keySz;
    }
    encryptedKeySz = (word32)keySz;

    /* Start of RecipientEncryptedKeys */

    /* EncryptedKey */
    encryptedKeyOctetSz = SetOctetString(encryptedKeySz, encryptedKeyOctet);
    totalSz += (encryptedKeyOctetSz + encryptedKeySz);

    /* SubjectKeyIdentifier */
    subjKeyIdOctetSz = SetOctetString(KEYID_SIZE, subjKeyIdOctet);
    totalSz += (subjKeyIdOctetSz + KEYID_SIZE);

    /* RecipientKeyIdentifier IMPLICIT [0] */
    recipKeyIdSeqSz = SetImplicit(ASN_SEQUENCE, 0, subjKeyIdOctetSz +
                                  KEYID_SIZE, recipKeyIdSeq);
    totalSz += recipKeyIdSeqSz;

    /* RecipientEncryptedKey */
    recipEncKeySeqSz = SetSequence(totalSz, recipEncKeySeq);
    totalSz += recipEncKeySeqSz;

    /* RecipientEncryptedKeys */
    recipEncKeysSeqSz = SetSequence(totalSz, recipEncKeysSeq);
    totalSz += recipEncKeysSeqSz;

    /* Start of optional UserKeyingMaterial */

    if (kari->ukmSz > 0) {
        ukmOctetSz = SetOctetString(kari->ukmSz, ukmOctetStr);
        totalSz += (ukmOctetSz + kari->ukmSz);

        ukmExplicitSz = SetExplicit(1, ukmOctetSz + kari->ukmSz,
                                    ukmExplicitSeq);
        totalSz += ukmExplicitSz;
    }

    /* Start of KeyEncryptionAlgorithmIdentifier */

    /* KeyWrapAlgorithm */
    keyWrapAlgSz = SetAlgoID(keyWrapOID, keyWrapAlg, oidKeyWrapType, 0);
    totalSz += keyWrapAlgSz;

    /* KeyEncryptionAlgorithmIdentifier */
    keyEncryptAlgoIdSz = SetAlgoID(keyAgreeOID, keyEncryptAlgoId,
                                   oidCmsKeyAgreeType, keyWrapAlgSz);
    totalSz += keyEncryptAlgoIdSz;

    /* Start of OriginatorIdentifierOrKey */

    /* recipient ECPoint, public key */
    XMEMSET(origPubKeyStr, 0, sizeof(origPubKeyStr)); /* no unused bits */
    origPubKeyStr[0] = ASN_BIT_STRING;
    origPubKeyStrSz = SetLength(kari->senderKeyExportSz + 1,
                                origPubKeyStr + 1) + 2;
    totalSz += (origPubKeyStrSz + kari->senderKeyExportSz);

    /* Originator AlgorithmIdentifier */
    origAlgIdSz = SetAlgoID(ECDSAk, origAlgId, oidKeyType, 0);
    totalSz += origAlgIdSz;

    /* outer OriginatorPublicKey IMPLICIT [1] */
    origPubKeySeqSz = SetImplicit(ASN_SEQUENCE, 1,
                                  origAlgIdSz + origPubKeyStrSz +
                                  kari->senderKeyExportSz, origPubKeySeq);
    totalSz += origPubKeySeqSz;

    /* outer OriginatorIdentiferOrKey IMPLICIT [0] */
    origIdOrKeySeqSz = SetImplicit(ASN_SEQUENCE, 0,
                                   origPubKeySeqSz + origAlgIdSz +
                                   origPubKeyStrSz + kari->senderKeyExportSz,
                                   origIdOrKeySeq);
    totalSz += origIdOrKeySeqSz;

    /* version, always 3 */
    verSz = SetMyVersion(3, ver, 0);
    totalSz += verSz;
    recip->recipVersion = 3;

    /* outer IMPLICIT [1] kari */
    kariSeqSz = SetImplicit(ASN_SEQUENCE, 1, totalSz, kariSeq);
    totalSz += kariSeqSz;

    if (totalSz > MAX_RECIP_SZ) {
        WOLFSSL_MSG("KeyAgreeRecipientInfo output buffer too small");
        wc_PKCS7_KariFree(kari);
#ifdef WOLFSSL_SMALL_STACK
        XFREE(encryptedKey, pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
#endif
        XFREE(recip, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        return BUFFER_E;
    }

    XMEMCPY(recip->recip + idx, kariSeq, kariSeqSz);
    idx += kariSeqSz;
    XMEMCPY(recip->recip + idx, ver, verSz);
    idx += verSz;

    XMEMCPY(recip->recip + idx, origIdOrKeySeq, origIdOrKeySeqSz);
    idx += origIdOrKeySeqSz;
    XMEMCPY(recip->recip + idx, origPubKeySeq, origPubKeySeqSz);
    idx += origPubKeySeqSz;
    XMEMCPY(recip->recip + idx, origAlgId, origAlgIdSz);
    idx += origAlgIdSz;
    XMEMCPY(recip->recip + idx, origPubKeyStr, origPubKeyStrSz);
    idx += origPubKeyStrSz;
    /* ephemeral public key */
    XMEMCPY(recip->recip + idx, kari->senderKeyExport, kari->senderKeyExportSz);
    idx += kari->senderKeyExportSz;

    if (kari->ukmSz > 0) {
        XMEMCPY(recip->recip + idx, ukmExplicitSeq, ukmExplicitSz);
        idx += ukmExplicitSz;
        XMEMCPY(recip->recip + idx, ukmOctetStr, ukmOctetSz);
        idx += ukmOctetSz;
        XMEMCPY(recip->recip + idx, kari->ukm, kari->ukmSz);
        idx += kari->ukmSz;
    }

    XMEMCPY(recip->recip + idx, keyEncryptAlgoId, keyEncryptAlgoIdSz);
    idx += keyEncryptAlgoIdSz;
    XMEMCPY(recip->recip + idx, keyWrapAlg, keyWrapAlgSz);
    idx += keyWrapAlgSz;

    XMEMCPY(recip->recip + idx, recipEncKeysSeq, recipEncKeysSeqSz);
    idx += recipEncKeysSeqSz;
    XMEMCPY(recip->recip + idx, recipEncKeySeq, recipEncKeySeqSz);
    idx += recipEncKeySeqSz;
    XMEMCPY(recip->recip + idx, recipKeyIdSeq, recipKeyIdSeqSz);
    idx += recipKeyIdSeqSz;
    XMEMCPY(recip->recip + idx, subjKeyIdOctet, subjKeyIdOctetSz);
    idx += subjKeyIdOctetSz;
    /* subject key id */
    XMEMCPY(recip->recip + idx, kari->decoded->extSubjKeyId, KEYID_SIZE);
    idx += KEYID_SIZE;
    XMEMCPY(recip->recip + idx, encryptedKeyOctet, encryptedKeyOctetSz);
    idx += encryptedKeyOctetSz;
    /* encrypted CEK */
    XMEMCPY(recip->recip + idx, encryptedKey, encryptedKeySz);
    idx += encryptedKeySz;

    wc_PKCS7_KariFree(kari);
#ifdef WOLFSSL_SMALL_STACK
    XFREE(encryptedKey, pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
#endif

    /* store recipient size */
    recip->recipSz = idx;
    recip->recipType = PKCS7_KARI;

    /* add recipient to recip list */
    if (pkcs7->recipList == NULL) {
        pkcs7->recipList = recip;
    } else {
        lastRecip = pkcs7->recipList;
        while (lastRecip->next != NULL) {
            lastRecip = lastRecip->next;
        }
        lastRecip->next = recip;
    }

    (void)options;

    return idx;
}

#endif /* HAVE_ECC */

#ifndef NO_RSA

/* Encode and add CMS EnvelopedData KTRI (KeyTransRecipientInfo) RecipientInfo
 * to CMS/PKCS#7 EnvelopedData structure.
 *
 * Returns 0 on success, negative upon error */
int wc_PKCS7_AddRecipient_KTRI(PKCS7* pkcs7, const byte* cert, word32 certSz,
                               int options)
{
    Pkcs7EncodedRecip* recip = NULL;
    Pkcs7EncodedRecip* lastRecip = NULL;

    WC_RNG rng;
    word32 idx = 0;
    word32 encryptedKeySz = 0;

    int ret = 0, blockKeySz;
    int verSz = 0, issuerSz = 0, snSz = 0, keyEncAlgSz = 0;
    int issuerSeqSz = 0, recipSeqSz = 0, issuerSerialSeqSz = 0;
    int encKeyOctetStrSz;
    int sidType;

    byte ver[MAX_VERSION_SZ];
    byte issuerSerialSeq[MAX_SEQ_SZ];
    byte recipSeq[MAX_SEQ_SZ];
    byte issuerSeq[MAX_SEQ_SZ];
    byte encKeyOctetStr[MAX_OCTET_STR_SZ];

    byte issuerSKIDSeq[MAX_SEQ_SZ];
    byte issuerSKID[MAX_OCTET_STR_SZ];
    word32 issuerSKIDSeqSz = 0, issuerSKIDSz = 0;

#ifdef WOLFSSL_SMALL_STACK
    byte*   serial;
    byte*   keyAlgArray;
    byte*   encryptedKey;
    RsaKey* pubKey;
    DecodedCert* decoded;

    serial = (byte*)XMALLOC(MAX_SN_SZ, pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
    keyAlgArray = (byte*)XMALLOC(MAX_SN_SZ, pkcs7->heap,
                                 DYNAMIC_TYPE_TMP_BUFFER);
    encryptedKey = (byte*)XMALLOC(MAX_ENCRYPTED_KEY_SZ, pkcs7->heap,
                                  DYNAMIC_TYPE_TMP_BUFFER);
    decoded = (DecodedCert*)XMALLOC(sizeof(DecodedCert), pkcs7->heap,
                                    DYNAMIC_TYPE_TMP_BUFFER);

    if (decoded == NULL || serial == NULL ||
        encryptedKey == NULL || keyAlgArray == NULL) {
        if (serial)
            XFREE(serial, pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
        if (keyAlgArray)
            XFREE(keyAlgArray, pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
        if (encryptedKey)
            XFREE(encryptedKey, pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
        if (decoded)
            XFREE(decoded, pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
        return MEMORY_E;
    }
#else
    byte serial[MAX_SN_SZ];
    byte keyAlgArray[MAX_ALGO_SZ];
    byte encryptedKey[MAX_ENCRYPTED_KEY_SZ];

    RsaKey pubKey[1];
    DecodedCert decoded[1];
#endif

    encryptedKeySz = MAX_ENCRYPTED_KEY_SZ;
    XMEMSET(encryptedKey, 0, encryptedKeySz);

    /* default to IssuerAndSerialNumber if not set */
    if (pkcs7->sidType != 0) {
        sidType = pkcs7->sidType;
    } else {
        sidType = CMS_ISSUER_AND_SERIAL_NUMBER;
    }

    /* allow options to override SubjectIdentifier type if set */
    if (options & CMS_SKID) {
        sidType = CMS_SKID;
    } else if (options & CMS_ISSUER_AND_SERIAL_NUMBER) {
        sidType = CMS_ISSUER_AND_SERIAL_NUMBER;
    }

    /* allocate recipient struct */
    recip = (Pkcs7EncodedRecip*)XMALLOC(sizeof(Pkcs7EncodedRecip), pkcs7->heap,
                                 DYNAMIC_TYPE_PKCS7);
    if (recip == NULL) {
#ifdef WOLFSSL_SMALL_STACK
        XFREE(serial,       pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
        XFREE(keyAlgArray,  pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
        XFREE(encryptedKey, pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
        XFREE(decoded,      pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
#endif
        return MEMORY_E;
    }
    XMEMSET(recip, 0, sizeof(Pkcs7EncodedRecip));

    /* get key size for content-encryption key based on algorithm */
    blockKeySz = wc_PKCS7_GetOIDKeySize(pkcs7->encryptOID);
    if (blockKeySz < 0) {
#ifdef WOLFSSL_SMALL_STACK
        XFREE(serial,       pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
        XFREE(keyAlgArray,  pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
        XFREE(encryptedKey, pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
        XFREE(decoded,      pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
#endif
        XFREE(recip, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        return blockKeySz;
    }

    /* generate random content encryption key, if needed */
    ret = PKCS7_GenerateContentEncryptionKey(pkcs7, blockKeySz);
    if (ret < 0) {
#ifdef WOLFSSL_SMALL_STACK
        XFREE(serial,       pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
        XFREE(keyAlgArray,  pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
        XFREE(encryptedKey, pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
        XFREE(decoded,      pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
#endif
        XFREE(recip, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        return ret;
    }

    InitDecodedCert(decoded, (byte*)cert, certSz, pkcs7->heap);
    ret = ParseCert(decoded, CA_TYPE, NO_VERIFY, 0);
    if (ret < 0) {
        FreeDecodedCert(decoded);
#ifdef WOLFSSL_SMALL_STACK
        XFREE(serial,       pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
        XFREE(keyAlgArray,  pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
        XFREE(encryptedKey, pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
        XFREE(decoded,      pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
#endif
        XFREE(recip, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        return ret;
    }

    if (sidType == CMS_ISSUER_AND_SERIAL_NUMBER) {

        /* version, must be 0 for IssuerAndSerialNumber */
        verSz = SetMyVersion(0, ver, 0);
        recip->recipVersion = 0;

        /* IssuerAndSerialNumber */
        if (decoded->issuerRaw == NULL || decoded->issuerRawLen == 0) {
            WOLFSSL_MSG("DecodedCert lacks raw issuer pointer and length");
            FreeDecodedCert(decoded);
#ifdef WOLFSSL_SMALL_STACK
            XFREE(serial,       pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
            XFREE(keyAlgArray,  pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
            XFREE(encryptedKey, pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
            XFREE(decoded,      pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
#endif
            XFREE(recip, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
            return -1;
        }
        issuerSz    = decoded->issuerRawLen;
        issuerSeqSz = SetSequence(issuerSz, issuerSeq);

        if (decoded->serialSz == 0) {
            WOLFSSL_MSG("DecodedCert missing serial number");
            FreeDecodedCert(decoded);
#ifdef WOLFSSL_SMALL_STACK
            XFREE(serial,       pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
            XFREE(keyAlgArray,  pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
            XFREE(encryptedKey, pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
            XFREE(decoded,      pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
#endif
            XFREE(recip, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
            return -1;
        }
        snSz = SetSerialNumber(decoded->serial, decoded->serialSz, serial,
                               MAX_SN_SZ);

        issuerSerialSeqSz = SetSequence(issuerSeqSz + issuerSz + snSz,
                                        issuerSerialSeq);

    } else if (sidType == CMS_SKID) {

        /* version, must be 2 for SubjectKeyIdentifier */
        verSz = SetMyVersion(2, ver, 0);
        recip->recipVersion = 2;

        issuerSKIDSz = SetOctetString(KEYID_SIZE, issuerSKID);
        issuerSKIDSeqSz = SetExplicit(0, issuerSKIDSz + KEYID_SIZE,
                                      issuerSKIDSeq);
    } else {
        return PKCS7_RECIP_E;
    }

    pkcs7->publicKeyOID = decoded->keyOID;

    /* KeyEncryptionAlgorithmIdentifier, only support RSA now */
    if (pkcs7->publicKeyOID != RSAk) {
        FreeDecodedCert(decoded);
#ifdef WOLFSSL_SMALL_STACK
        XFREE(serial,       pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
        XFREE(keyAlgArray,  pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
        XFREE(encryptedKey, pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
        XFREE(decoded,      pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
#endif
        XFREE(recip, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        return ALGO_ID_E;
    }

    keyEncAlgSz = SetAlgoID(pkcs7->publicKeyOID, keyAlgArray, oidKeyType, 0);
    if (keyEncAlgSz == 0) {
        FreeDecodedCert(decoded);
#ifdef WOLFSSL_SMALL_STACK
        XFREE(serial,       pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
        XFREE(keyAlgArray,  pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
        XFREE(encryptedKey, pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
        XFREE(decoded,      pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
#endif
        XFREE(recip, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        return BAD_FUNC_ARG;
    }

#ifdef WOLFSSL_SMALL_STACK
    pubKey = (RsaKey*)XMALLOC(sizeof(RsaKey), pkcs7->heap,
            DYNAMIC_TYPE_TMP_BUFFER);
    if (pubKey == NULL) {
        FreeDecodedCert(decoded);
        XFREE(serial,       pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
        XFREE(keyAlgArray,  pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
        XFREE(encryptedKey, pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
        XFREE(decoded,      pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
        XFREE(recip, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        return MEMORY_E;
    }
#endif

    /* EncryptedKey */
    ret = wc_InitRsaKey_ex(pubKey, pkcs7->heap, INVALID_DEVID);
    if (ret != 0) {
        FreeDecodedCert(decoded);
#ifdef WOLFSSL_SMALL_STACK
        XFREE(pubKey,       pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
        XFREE(serial,       pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
        XFREE(keyAlgArray,  pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
        XFREE(encryptedKey, pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
        XFREE(decoded,      pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
#endif
        XFREE(recip, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        return ret;
    }

    if (wc_RsaPublicKeyDecode(decoded->publicKey, &idx, pubKey,
                              decoded->pubKeySize) < 0) {
        WOLFSSL_MSG("ASN RSA key decode error");
        wc_FreeRsaKey(pubKey);
        FreeDecodedCert(decoded);
#ifdef WOLFSSL_SMALL_STACK
        XFREE(pubKey,       pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
        XFREE(serial,       pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
        XFREE(keyAlgArray,  pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
        XFREE(encryptedKey, pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
        XFREE(decoded,      pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
#endif
        XFREE(recip, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        return PUBLIC_KEY_E;
    }

    ret = wc_InitRng_ex(&rng, pkcs7->heap, pkcs7->devId);
    if (ret != 0) {
        wc_FreeRsaKey(pubKey);
        FreeDecodedCert(decoded);
#ifdef WOLFSSL_SMALL_STACK
        XFREE(pubKey,       pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
        XFREE(serial,       pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
        XFREE(keyAlgArray,  pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
        XFREE(encryptedKey, pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
        XFREE(decoded,      pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
#endif
        XFREE(recip, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        return MEMORY_E;
    }


    ret = wc_RsaPublicEncrypt(pkcs7->cek, pkcs7->cekSz, encryptedKey,
                              encryptedKeySz, pubKey, &rng);
    wc_FreeRsaKey(pubKey);
    wc_FreeRng(&rng);

#ifdef WOLFSSL_SMALL_STACK
    XFREE(pubKey, pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
#endif

    if (ret < 0) {
        WOLFSSL_MSG("RSA Public Encrypt failed");
        FreeDecodedCert(decoded);
#ifdef WOLFSSL_SMALL_STACK
        XFREE(serial,       pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
        XFREE(keyAlgArray,  pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
        XFREE(encryptedKey, pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
        XFREE(decoded,      pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
#endif
        XFREE(recip, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        return ret;
    }
    encryptedKeySz = ret;

    encKeyOctetStrSz = SetOctetString(encryptedKeySz, encKeyOctetStr);

    /* RecipientInfo */
    if (sidType == CMS_ISSUER_AND_SERIAL_NUMBER) {
        recipSeqSz = SetSequence(verSz + issuerSerialSeqSz + issuerSeqSz +
                                 issuerSz + snSz + keyEncAlgSz +
                                 encKeyOctetStrSz + encryptedKeySz, recipSeq);

        if (recipSeqSz + verSz + issuerSerialSeqSz + issuerSeqSz + snSz +
            keyEncAlgSz + encKeyOctetStrSz + encryptedKeySz > MAX_RECIP_SZ) {
            WOLFSSL_MSG("RecipientInfo output buffer too small");
            FreeDecodedCert(decoded);
#ifdef WOLFSSL_SMALL_STACK
            XFREE(serial,       pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
            XFREE(keyAlgArray,  pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
            XFREE(encryptedKey, pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
            XFREE(decoded,      pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
#endif
            XFREE(recip, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
            return BUFFER_E;
        }

    } else {
        recipSeqSz = SetSequence(verSz + issuerSKIDSeqSz + issuerSKIDSz +
                                 KEYID_SIZE + keyEncAlgSz + encKeyOctetStrSz +
                                 encryptedKeySz, recipSeq);

        if (recipSeqSz + verSz + issuerSKIDSeqSz + issuerSKIDSz + KEYID_SIZE +
            keyEncAlgSz + encKeyOctetStrSz + encryptedKeySz > MAX_RECIP_SZ) {
            WOLFSSL_MSG("RecipientInfo output buffer too small");
            FreeDecodedCert(decoded);
#ifdef WOLFSSL_SMALL_STACK
            XFREE(serial,       pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
            XFREE(keyAlgArray,  pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
            XFREE(encryptedKey, pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
            XFREE(decoded,      pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
#endif
            XFREE(recip, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
            return BUFFER_E;
        }
    }

    idx = 0;
    XMEMCPY(recip->recip + idx, recipSeq, recipSeqSz);
    idx += recipSeqSz;
    XMEMCPY(recip->recip + idx, ver, verSz);
    idx += verSz;
    if (sidType == CMS_ISSUER_AND_SERIAL_NUMBER) {
        XMEMCPY(recip->recip + idx, issuerSerialSeq, issuerSerialSeqSz);
        idx += issuerSerialSeqSz;
        XMEMCPY(recip->recip + idx, issuerSeq, issuerSeqSz);
        idx += issuerSeqSz;
        XMEMCPY(recip->recip + idx, decoded->issuerRaw, issuerSz);
        idx += issuerSz;
        XMEMCPY(recip->recip + idx, serial, snSz);
        idx += snSz;
    } else {
        XMEMCPY(recip->recip + idx, issuerSKIDSeq, issuerSKIDSeqSz);
        idx += issuerSKIDSeqSz;
        XMEMCPY(recip->recip + idx, issuerSKID, issuerSKIDSz);
        idx += issuerSKIDSz;
        XMEMCPY(recip->recip + idx, pkcs7->issuerSubjKeyId, KEYID_SIZE);
        idx += KEYID_SIZE;
    }
    XMEMCPY(recip->recip + idx, keyAlgArray, keyEncAlgSz);
    idx += keyEncAlgSz;
    XMEMCPY(recip->recip + idx, encKeyOctetStr, encKeyOctetStrSz);
    idx += encKeyOctetStrSz;
    XMEMCPY(recip->recip + idx, encryptedKey, encryptedKeySz);
    idx += encryptedKeySz;

    FreeDecodedCert(decoded);

#ifdef WOLFSSL_SMALL_STACK
    XFREE(serial,       pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
    XFREE(keyAlgArray,  pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
    XFREE(encryptedKey, pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
    XFREE(decoded,      pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
#endif

    /* store recipient size */
    recip->recipSz = idx;
    recip->recipType = PKCS7_KTRI;

    /* add recipient to recip list */
    if (pkcs7->recipList == NULL) {
        pkcs7->recipList = recip;
    } else {
        lastRecip = pkcs7->recipList;
        while (lastRecip->next != NULL) {
            lastRecip = lastRecip->next;
        }
        lastRecip->next = recip;
    }

    return idx;
}

#endif /* !NO_RSA */


/* encrypt content using encryptOID algo */
static int wc_PKCS7_EncryptContent(int encryptOID, byte* key, int keySz,
                                   byte* iv, int ivSz, byte* aad, word32 aadSz,
                                   byte* authTag, word32 authTagSz, byte* in,
                                   int inSz, byte* out)
{
    int ret;
#ifndef NO_AES
    Aes  aes;
#endif
#ifndef NO_DES3
    Des  des;
    Des3 des3;
#endif

    if (key == NULL || iv == NULL || in == NULL || out == NULL)
        return BAD_FUNC_ARG;

    switch (encryptOID) {
#ifndef NO_AES
    #ifdef WOLFSSL_AES_128
        case AES128CBCb:
    #endif
    #ifdef WOLFSSL_AES_192
        case AES192CBCb:
    #endif
    #ifdef WOLFSSL_AES_256
        case AES256CBCb:
    #endif
            if (
                #ifdef WOLFSSL_AES_128
                    (encryptOID == AES128CBCb && keySz != 16 ) ||
                #endif
                #ifdef WOLFSSL_AES_192
                    (encryptOID == AES192CBCb && keySz != 24 ) ||
                #endif
                #ifdef WOLFSSL_AES_256
                    (encryptOID == AES256CBCb && keySz != 32 ) ||
                #endif
                    (ivSz  != AES_BLOCK_SIZE) )
                return BAD_FUNC_ARG;

            ret = wc_AesSetKey(&aes, key, keySz, iv, AES_ENCRYPTION);
            if (ret == 0)
                ret = wc_AesCbcEncrypt(&aes, out, in, inSz);

            break;
    #ifdef HAVE_AESGCM
        #ifdef WOLFSSL_AES_128
        case AES128GCMb:
        #endif
        #ifdef WOLFSSL_AES_192
        case AES192GCMb:
        #endif
        #ifdef WOLFSSL_AES_256
        case AES256GCMb:
        #endif
        #if defined(WOLFSSL_AES_128) || defined(WOLFSSL_AES_192) || \
            defined(WOLFSSL_AES_256)
            if (authTag == NULL)
                return BAD_FUNC_ARG;

            ret = wc_AesGcmSetKey(&aes, key, keySz);
            if (ret == 0)
                ret = wc_AesGcmEncrypt(&aes, out, in, inSz, iv, ivSz,
                                       authTag, authTagSz, aad, aadSz);
            break;
        #endif
    #endif /* HAVE_AESGCM */
    #ifdef HAVE_AESCCM
        #ifdef WOLFSSL_AES_128
        case AES128CCMb:
        #endif
        #ifdef WOLFSSL_AES_192
        case AES192CCMb:
        #endif
        #ifdef WOLFSSL_AES_256
        case AES256CCMb:
        #endif
        #if defined(WOLFSSL_AES_128) || defined(WOLFSSL_AES_192) || \
            defined(WOLFSSL_AES_256)
            if (authTag == NULL)
                return BAD_FUNC_ARG;

            ret = wc_AesCcmSetKey(&aes, key, keySz);
            if (ret == 0)
                ret = wc_AesCcmEncrypt(&aes, out, in, inSz, iv, ivSz,
                                       authTag, authTagSz, aad, aadSz);
            break;
        #endif
    #endif /* HAVE_AESCCM */
#endif /* NO_AES */
#ifndef NO_DES3
        case DESb:
            if (keySz != DES_KEYLEN || ivSz != DES_BLOCK_SIZE)
                return BAD_FUNC_ARG;

            ret = wc_Des_SetKey(&des, key, iv, DES_ENCRYPTION);
            if (ret == 0)
                ret = wc_Des_CbcEncrypt(&des, out, in, inSz);

            break;

        case DES3b:
            if (keySz != DES3_KEYLEN || ivSz != DES_BLOCK_SIZE)
                return BAD_FUNC_ARG;

            ret = wc_Des3_SetKey(&des3, key, iv, DES_ENCRYPTION);
            if (ret == 0)
                ret = wc_Des3_CbcEncrypt(&des3, out, in, inSz);

            break;
#endif
        default:
            WOLFSSL_MSG("Unsupported content cipher type");
            return ALGO_ID_E;
    };

#if defined(NO_AES) || (!defined(HAVE_AESGCM) && !defined(HAVE_AESCCM))
    (void)authTag;
    (void)authTagSz;
    (void)aad;
    (void)aadSz;
#endif
    return ret;
}


/* decrypt content using encryptOID algo */
static int wc_PKCS7_DecryptContent(int encryptOID, byte* key, int keySz,
                                   byte* iv, int ivSz, byte* aad, word32 aadSz,
                                   byte* authTag, word32 authTagSz, byte* in,
                                   int inSz, byte* out)
{
    int ret;
#ifndef NO_AES
    Aes  aes;
#endif
#ifndef NO_DES3
    Des  des;
    Des3 des3;
#endif

    if (key == NULL || iv == NULL || in == NULL || out == NULL)
        return BAD_FUNC_ARG;

    switch (encryptOID) {
#ifndef NO_AES
    #ifdef WOLFSSL_AES_128
        case AES128CBCb:
    #endif
    #ifdef WOLFSSL_AES_192
        case AES192CBCb:
    #endif
    #ifdef WOLFSSL_AES_256
        case AES256CBCb:
    #endif
            if (
                #ifdef WOLFSSL_AES_128
                    (encryptOID == AES128CBCb && keySz != 16 ) ||
                #endif
                #ifdef WOLFSSL_AES_192
                    (encryptOID == AES192CBCb && keySz != 24 ) ||
                #endif
                #ifdef WOLFSSL_AES_256
                    (encryptOID == AES256CBCb && keySz != 32 ) ||
                #endif
                    (ivSz  != AES_BLOCK_SIZE) )
                return BAD_FUNC_ARG;

            ret = wc_AesSetKey(&aes, key, keySz, iv, AES_DECRYPTION);
            if (ret == 0)
                ret = wc_AesCbcDecrypt(&aes, out, in, inSz);

            break;
    #ifdef HAVE_AESGCM
        #ifdef WOLFSSL_AES_128
        case AES128GCMb:
        #endif
        #ifdef WOLFSSL_AES_192
        case AES192GCMb:
        #endif
        #ifdef WOLFSSL_AES_256
        case AES256GCMb:
        #endif
        #if defined(WOLFSSL_AES_128) || defined(WOLFSSL_AES_192) || \
            defined(WOLFSSL_AES_256)
            if (authTag == NULL)
                return BAD_FUNC_ARG;

            ret = wc_AesGcmSetKey(&aes, key, keySz);
            if (ret == 0)
                ret = wc_AesGcmDecrypt(&aes, out, in, inSz, iv, ivSz,
                                       authTag, authTagSz, aad, aadSz);
            break;
        #endif
    #endif /* HAVE_AESGCM */
    #ifdef HAVE_AESCCM
        #ifdef WOLFSSL_AES_128
        case AES128CCMb:
        #endif
        #ifdef WOLFSSL_AES_192
        case AES192CCMb:
        #endif
        #ifdef WOLFSSL_AES_256
        case AES256CCMb:
        #endif
        #if defined(WOLFSSL_AES_128) || defined(WOLFSSL_AES_192) || \
            defined(WOLFSSL_AES_256)
            if (authTag == NULL)
                return BAD_FUNC_ARG;

            ret = wc_AesCcmSetKey(&aes, key, keySz);
            if (ret == 0)
                ret = wc_AesCcmDecrypt(&aes, out, in, inSz, iv, ivSz,
                                       authTag, authTagSz, aad, aadSz);
            break;
        #endif
    #endif /* HAVE_AESCCM */
#endif /* NO_AES */
#ifndef NO_DES3
        case DESb:
            if (keySz != DES_KEYLEN || ivSz != DES_BLOCK_SIZE)
                return BAD_FUNC_ARG;

            ret = wc_Des_SetKey(&des, key, iv, DES_DECRYPTION);
            if (ret == 0)
                ret = wc_Des_CbcDecrypt(&des, out, in, inSz);

            break;
        case DES3b:
            if (keySz != DES3_KEYLEN || ivSz != DES_BLOCK_SIZE)
                return BAD_FUNC_ARG;

            ret = wc_Des3_SetKey(&des3, key, iv, DES_DECRYPTION);
            if (ret == 0)
                ret = wc_Des3_CbcDecrypt(&des3, out, in, inSz);

            break;
#endif
        default:
            WOLFSSL_MSG("Unsupported content cipher type");
            return ALGO_ID_E;
    };

#if defined(NO_AES) || (!defined(HAVE_AESGCM) && !defined(HAVE_AESCCM))
    (void)authTag;
    (void)authTagSz;
    (void)aad;
    (void)aadSz;
#endif

    return ret;
}


/* Generate random block, place in out, return 0 on success negative on error.
 * Used for generation of IV, nonce, etc */
static int wc_PKCS7_GenerateBlock(PKCS7* pkcs7, WC_RNG* rng, byte* out,
                                  word32 outSz)
{
    int ret;
    WC_RNG* rnd = NULL;

    if (out == NULL || outSz == 0)
        return BAD_FUNC_ARG;

    /* input RNG is optional, init local one if input rng is NULL */
    if (rng == NULL) {
        rnd = (WC_RNG*)XMALLOC(sizeof(WC_RNG), pkcs7->heap, DYNAMIC_TYPE_RNG);
        if (rnd == NULL)
            return MEMORY_E;

        ret = wc_InitRng_ex(rnd, pkcs7->heap, pkcs7->devId);
        if (ret != 0) {
            XFREE(rnd, pkcs7->heap, DYNAMIC_TYPE_RNG);
            return ret;
        }

    } else {
        rnd = rng;
    }

    ret = wc_RNG_GenerateBlock(rnd, out, outSz);

    if (rng == NULL) {
        wc_FreeRng(rnd);
        XFREE(rnd, pkcs7->heap, DYNAMIC_TYPE_RNG);
    }

    return ret;
}


/* Set default SignerIdentifier type to be used. Is either
 * IssuerAndSerialNumber or SubjectKeyIdentifier. Encoding defaults to using
 * IssuerAndSerialNumber unless set with this function or explicitly
 * overriden via options when adding RecipientInfo type.
 *
 * pkcs7 - pointer to initialized PKCS7 structure
 * type  - either CMS_ISSUER_AND_SERIAL_NUMBER or CMS_SKID
 *
 * return 0 on success, negative upon error */
int wc_PKCS7_SetSignerIdentifierType(PKCS7* pkcs7, int type)
{
    if (pkcs7 == NULL)
        return BAD_FUNC_ARG;

    if (type != CMS_ISSUER_AND_SERIAL_NUMBER &&
        type != CMS_SKID) {
        return BAD_FUNC_ARG;
    }

    pkcs7->sidType = type;

    return 0;
}


/* Set custom contentType, currently supported with SignedData type
 *
 * pkcs7       - pointer to initialized PKCS7 structure
 * contentType - pointer to array with ASN.1 encoded OID value
 * sz          - length of contentType array, octets
 *
 * return 0 on success, negative upon error */
int wc_PKCS7_SetContentType(PKCS7* pkcs7, byte* contentType, word32 sz)
{
    if (pkcs7 == NULL || contentType == NULL || sz == 0)
        return BAD_FUNC_ARG;

    if (sz > MAX_OID_SZ) {
        WOLFSSL_MSG("input array too large, bounded by MAX_OID_SZ");
        return BAD_FUNC_ARG;
    }

    XMEMCPY(pkcs7->contentType, contentType, sz);
    pkcs7->contentTypeSz = sz;

    return 0;
}


/* return size of padded data, padded to blockSz chunks, or negative on error */
int wc_PKCS7_GetPadSize(word32 inputSz, word32 blockSz)
{
    int padSz;

    if (blockSz == 0)
        return BAD_FUNC_ARG;

    padSz = blockSz - (inputSz % blockSz);

    return padSz;
}


/* pad input data to blockSz chunk, place in outSz. out must be big enough
 * for input + pad bytes. See wc_PKCS7_GetPadSize() helper. */
int wc_PKCS7_PadData(byte* in, word32 inSz, byte* out, word32 outSz,
                     word32 blockSz)
{
    int i, padSz;

    if (in == NULL  || inSz == 0 ||
        out == NULL || outSz == 0)
        return BAD_FUNC_ARG;

    padSz = wc_PKCS7_GetPadSize(inSz, blockSz);

    if (outSz < (inSz + padSz))
        return BAD_FUNC_ARG;

    XMEMCPY(out, in, inSz);

    for (i = 0; i < padSz; i++) {
        out[inSz + i] = (byte)padSz;
    }

    return inSz + padSz;
}


/* Encode and add CMS EnvelopedData ORI (OtherRecipientInfo) RecipientInfo
 * to CMS/PKCS#7 EnvelopedData structure.
 *
 * Return 0 on success, negative upon error */
int wc_PKCS7_AddRecipient_ORI(PKCS7* pkcs7, CallbackOriEncrypt oriEncryptCb,
                              int options)
{
    int oriTypeLenSz, blockKeySz, ret;
    word32 idx, recipSeqSz;

    Pkcs7EncodedRecip* recip = NULL;
    Pkcs7EncodedRecip* lastRecip = NULL;

    byte recipSeq[MAX_SEQ_SZ];
    byte oriTypeLen[MAX_LENGTH_SZ];

    byte oriType[MAX_ORI_TYPE_SZ];
    byte oriValue[MAX_ORI_VALUE_SZ];
    word32 oriTypeSz = MAX_ORI_TYPE_SZ;
    word32 oriValueSz = MAX_ORI_VALUE_SZ;

    if (pkcs7 == NULL || oriEncryptCb == NULL) {
        return BAD_FUNC_ARG;
    }

    /* allocate memory for RecipientInfo, KEK, encrypted key */
    recip = (Pkcs7EncodedRecip*)XMALLOC(sizeof(Pkcs7EncodedRecip),
                                        pkcs7->heap, DYNAMIC_TYPE_PKCS7);
    if (recip == NULL)
        return MEMORY_E;
    XMEMSET(recip, 0, sizeof(Pkcs7EncodedRecip));

    /* get key size for content-encryption key based on algorithm */
    blockKeySz = wc_PKCS7_GetOIDKeySize(pkcs7->encryptOID);
    if (blockKeySz < 0) {
        XFREE(recip, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        return blockKeySz;
    }

    /* generate random content encryption key, if needed */
    ret = PKCS7_GenerateContentEncryptionKey(pkcs7, blockKeySz);
    if (ret < 0) {
        XFREE(recip, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        return ret;
    }

    /* call user callback to encrypt CEK and get oriType and oriValue
       values back */
    ret = oriEncryptCb(pkcs7, pkcs7->cek, pkcs7->cekSz, oriType, &oriTypeSz,
                       oriValue, &oriValueSz, pkcs7->oriEncryptCtx);
    if (ret != 0) {
        XFREE(recip, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        return ret;
    }

    oriTypeLenSz = SetLength(oriTypeSz, oriTypeLen);

    recipSeqSz = SetImplicit(ASN_SEQUENCE, 4, 1 + oriTypeLenSz + oriTypeSz +
                             oriValueSz, recipSeq);

    idx = 0;
    XMEMCPY(recip->recip + idx, recipSeq, recipSeqSz);
    idx += recipSeqSz;
    /* oriType */
    recip->recip[idx] = ASN_OBJECT_ID;
    idx += 1;
    XMEMCPY(recip->recip + idx, oriTypeLen, oriTypeLenSz);
    idx += oriTypeLenSz;
    XMEMCPY(recip->recip + idx, oriType, oriTypeSz);
    idx += oriTypeSz;
    /* oriValue, input MUST already be ASN.1 encoded */
    XMEMCPY(recip->recip + idx, oriValue, oriValueSz);
    idx += oriValueSz;

    /* store recipient size */
    recip->recipSz = idx;
    recip->recipType = PKCS7_ORI;
    recip->recipVersion = 4;

    /* add recipient to recip list */
    if (pkcs7->recipList == NULL) {
        pkcs7->recipList = recip;
    } else {
        lastRecip = pkcs7->recipList;
        while (lastRecip->next != NULL) {
            lastRecip = lastRecip->next;
        }
        lastRecip->next = recip;
    }

    (void)options;

    return idx;
}

#ifndef NO_PWDBASED


static int wc_PKCS7_GenerateKEK_PWRI(PKCS7* pkcs7, byte* passwd, word32 pLen,
                                     byte* salt, word32 saltSz, int kdfOID,
                                     int prfOID, int iterations, byte* out,
                                     word32 outSz)
{
    int ret;

    if (pkcs7 == NULL || passwd == NULL || salt == NULL || out == NULL)
        return BAD_FUNC_ARG;

    switch (kdfOID) {

        case PBKDF2_OID:

            ret = wc_PBKDF2(out, passwd, pLen, salt, saltSz, iterations,
                            outSz, prfOID);
            if (ret != 0) {
                return ret;
            }

            break;

        default:
            WOLFSSL_MSG("Unsupported KDF OID");
            return PKCS7_OID_E;
    }

    return 0;
}


/* RFC3211 (Section 2.3.1) key wrap algorithm (id-alg-PWRI-KEK).
 *
 * Returns output size on success, negative upon error */
static int wc_PKCS7_PwriKek_KeyWrap(PKCS7* pkcs7, const byte* kek, word32 kekSz,
                                    const byte* cek, word32 cekSz,
                                    byte* out, word32 *outSz,
                                    const byte* iv, word32 ivSz, int algID)
{
    WC_RNG rng;
    int blockSz, outLen, ret;
    word32 padSz;
    byte* lastBlock;

    if (kek == NULL || cek == NULL || iv == NULL || outSz == NULL)
        return BAD_FUNC_ARG;

    /* get encryption algorithm block size */
    blockSz = wc_PKCS7_GetOIDBlockSize(algID);
    if (blockSz < 0)
        return blockSz;

    /* get pad bytes needed to block boundary */
    padSz = blockSz - ((4 + cekSz) % blockSz);
    outLen = 4 + cekSz + padSz;

    /* must be at least two blocks long */
    if (outLen < 2 * blockSz)
        padSz += blockSz;

    /* if user set out to NULL, give back required length */
    if (out == NULL) {
        *outSz = outLen;
        return LENGTH_ONLY_E;
    }

    /* verify output buffer is large enough */
    if (*outSz < (word32)outLen)
        return BUFFER_E;

    out[0] = cekSz;
    out[1] = ~cek[0];
    out[2] = ~cek[1];
    out[3] = ~cek[2];
    XMEMCPY(out + 4, cek, cekSz);

    /* random padding of size padSz */
    ret = wc_InitRng_ex(&rng, pkcs7->heap, pkcs7->devId);
    if (ret != 0)
        return ret;

    ret = wc_RNG_GenerateBlock(&rng, out + 4 + cekSz, padSz);

    if (ret == 0) {
        /* encrypt, normal */
        ret = wc_PKCS7_EncryptContent(algID, (byte*)kek, kekSz, (byte*)iv,
                                      ivSz, NULL, 0, NULL, 0, out, outLen, out);
    }

    if (ret == 0) {
        /* encrypt again, using last ciphertext block as IV */
        lastBlock = out + (((outLen / blockSz) - 1) * blockSz);
        ret = wc_PKCS7_EncryptContent(algID, (byte*)kek, kekSz, lastBlock,
                                      blockSz, NULL, 0, NULL, 0, out,
                                      outLen, out);
    }

    if (ret == 0) {
        *outSz = outLen;
    } else {
        outLen = ret;
    }

    wc_FreeRng(&rng);

    return outLen;
}


/* RFC3211 (Section 2.3.2) key unwrap algorithm (id-alg-PWRI-KEK).
 *
 * Returns cek size on success, negative upon error */
static int wc_PKCS7_PwriKek_KeyUnWrap(PKCS7* pkcs7, const byte* kek,
                                      word32 kekSz, const byte* in, word32 inSz,
                                      byte* out, word32 outSz, const byte* iv,
                                      word32 ivSz, int algID)
{
    int blockSz, cekLen, ret;
    byte* tmpIv     = NULL;
    byte* lastBlock = NULL;
    byte* outTmp    = NULL;

    if (pkcs7 == NULL || kek == NULL || in == NULL ||
        out == NULL || iv == NULL) {
        return BAD_FUNC_ARG;
    }

    outTmp = (byte*)XMALLOC(inSz, pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
    if (outTmp == NULL)
        return MEMORY_E;

    /* get encryption algorithm block size */
    blockSz = wc_PKCS7_GetOIDBlockSize(algID);
    if (blockSz < 0) {
        XFREE(outTmp, pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
        return blockSz;
    }

    /* input needs to be blockSz multiple and at least 2 * blockSz */
    if (((inSz % blockSz) != 0) || (inSz < (2 * (word32)blockSz))) {
        WOLFSSL_MSG("PWRI-KEK unwrap input must of block size and >= 2 "
                    "times block size");
        XFREE(outTmp, pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
        return BAD_FUNC_ARG;
    }

    /* use block out[n-1] as IV to decrypt block out[n] */
    lastBlock = (byte*)in + inSz - blockSz;
    tmpIv = lastBlock - blockSz;

    /* decrypt last block */
    ret = wc_PKCS7_DecryptContent(algID, (byte*)kek, kekSz, tmpIv, blockSz,
                                  NULL, 0, NULL, 0, lastBlock, blockSz,
                                  outTmp + inSz - blockSz);

    if (ret == 0) {
        /* using last decrypted block as IV, decrypt [0 ... n-1] blocks */
        lastBlock = outTmp + inSz - blockSz;
        ret = wc_PKCS7_DecryptContent(algID, (byte*)kek, kekSz, lastBlock,
                                      blockSz, NULL, 0, NULL, 0, (byte*)in,
                                      inSz - blockSz, outTmp);
    }

    if (ret == 0) {
        /* decrypt using original kek and iv */
        ret = wc_PKCS7_DecryptContent(algID, (byte*)kek, kekSz, (byte*)iv,
                                      ivSz, NULL, 0, NULL, 0, outTmp, inSz,
                                      outTmp);
    }

    if (ret != 0) {
        ForceZero(outTmp, inSz);
        XFREE(outTmp, pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
        return ret;
    }

    cekLen = outTmp[0];

    /* verify length */
    if ((word32)cekLen > inSz) {
        ForceZero(outTmp, inSz);
        XFREE(outTmp, pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
        return BAD_FUNC_ARG;
    }

    /* verify check bytes */
    if ((outTmp[1] ^ outTmp[4]) != 0xFF ||
        (outTmp[2] ^ outTmp[5]) != 0xFF ||
        (outTmp[3] ^ outTmp[6]) != 0xFF) {
        ForceZero(outTmp, inSz);
        XFREE(outTmp, pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
        return BAD_FUNC_ARG;
    }

    if (outSz < (word32)cekLen) {
        ForceZero(outTmp, inSz);
        XFREE(outTmp, pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
        return BUFFER_E;
    }

    XMEMCPY(out, outTmp + 4, outTmp[0]);
    ForceZero(outTmp, inSz);
    XFREE(outTmp, pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);

    return cekLen;
}


/* Encode and add CMS EnvelopedData PWRI (PasswordRecipientInfo) RecipientInfo
 * to CMS/PKCS#7 EnvelopedData structure.
 *
 * Return 0 on success, negative upon error */
int wc_PKCS7_AddRecipient_PWRI(PKCS7* pkcs7, byte* passwd, word32 pLen,
                               byte* salt, word32 saltSz, int kdfOID,
                               int hashOID, int iterations, int kekEncryptOID,
                               int options)
{
    Pkcs7EncodedRecip* recip = NULL;
    Pkcs7EncodedRecip* lastRecip = NULL;

    /* PasswordRecipientInfo */
    byte recipSeq[MAX_SEQ_SZ];
    byte ver[MAX_VERSION_SZ];
    word32 recipSeqSz, verSz;

    /* KeyDerivationAlgorithmIdentifier */
    byte kdfAlgoIdSeq[MAX_SEQ_SZ];
    byte kdfAlgoId[MAX_OID_SZ];
    byte kdfParamsSeq[MAX_SEQ_SZ];              /* PBKDF2-params */
    byte kdfSaltOctetStr[MAX_OCTET_STR_SZ];     /* salt OCTET STRING */
    byte kdfIterations[MAX_VERSION_SZ];
    word32 kdfAlgoIdSeqSz, kdfAlgoIdSz;
    word32 kdfParamsSeqSz, kdfSaltOctetStrSz, kdfIterationsSz;
    /* OPTIONAL: keyLength, not supported yet */
    /* OPTIONAL: prf AlgorithIdentifier, not supported yet */

    /* KeyEncryptionAlgorithmIdentifier */
    byte keyEncAlgoIdSeq[MAX_SEQ_SZ];
    byte keyEncAlgoId[MAX_OID_SZ];              /* id-alg-PWRI-KEK */
    byte pwriEncAlgoId[MAX_ALGO_SZ];
    byte ivOctetString[MAX_OCTET_STR_SZ];
    word32 keyEncAlgoIdSeqSz, keyEncAlgoIdSz;
    word32 pwriEncAlgoIdSz, ivOctetStringSz;

    /* EncryptedKey */
    byte encKeyOctetStr[MAX_OCTET_STR_SZ];
    word32 encKeyOctetStrSz;

    byte tmpIv[MAX_CONTENT_IV_SIZE];
    byte* encryptedKey = NULL;
    byte* kek = NULL;

    int cekKeySz = 0, kekKeySz = 0, kekBlockSz = 0, ret = 0;
    int encryptOID;
    word32 idx, totalSz = 0, encryptedKeySz;

    if (pkcs7 == NULL || passwd == NULL || pLen == 0 ||
        salt == NULL || saltSz == 0) {
        return BAD_FUNC_ARG;
    }

    /* allow user to use different KEK encryption algorithm than used for
     * main content encryption algorithm, if passed in */
    if (kekEncryptOID != 0) {
        encryptOID = kekEncryptOID;
    } else {
        encryptOID = pkcs7->encryptOID;
    }

    /* get content-encryption key size, based on algorithm */
    cekKeySz = wc_PKCS7_GetOIDKeySize(pkcs7->encryptOID);
    if (cekKeySz < 0)
        return cekKeySz;

    /* get KEK encryption key size, based on algorithm */
    if (encryptOID != pkcs7->encryptOID) {
        kekKeySz = wc_PKCS7_GetOIDKeySize(encryptOID);
    } else {
        kekKeySz = cekKeySz;
    }

    /* get KEK encryption block size */
    kekBlockSz = wc_PKCS7_GetOIDBlockSize(encryptOID);
    if (kekBlockSz < 0)
        return kekBlockSz;

    /* generate random CEK */
    ret = PKCS7_GenerateContentEncryptionKey(pkcs7, cekKeySz);
    if (ret < 0)
        return ret;

    /* generate random IV */
    ret = wc_PKCS7_GenerateBlock(pkcs7, NULL, tmpIv, kekBlockSz);
    if (ret != 0)
        return ret;

    /* allocate memory for RecipientInfo, KEK, encrypted key */
    recip = (Pkcs7EncodedRecip*)XMALLOC(sizeof(Pkcs7EncodedRecip),
                                        pkcs7->heap, DYNAMIC_TYPE_PKCS7);
    if (recip == NULL)
        return MEMORY_E;

    kek = (byte*)XMALLOC(kekKeySz, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
    if (kek == NULL) {
        XFREE(recip, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        return MEMORY_E;
    }

    encryptedKey = (byte*)XMALLOC(MAX_ENCRYPTED_KEY_SZ,
                                  pkcs7->heap, DYNAMIC_TYPE_PKCS7);
    if (encryptedKey == NULL) {
        XFREE(recip, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        XFREE(kek, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        return MEMORY_E;
    }

    encryptedKeySz = MAX_ENCRYPTED_KEY_SZ;
    XMEMSET(recip, 0, sizeof(Pkcs7EncodedRecip));
    XMEMSET(kek, 0, kekKeySz);
    XMEMSET(encryptedKey, 0, encryptedKeySz);

    /* generate KEK: expand password into KEK */
    ret = wc_PKCS7_GenerateKEK_PWRI(pkcs7, passwd, pLen, salt, saltSz,
                                    kdfOID, hashOID, iterations, kek,
                                    kekKeySz);
    if (ret < 0) {
        XFREE(recip, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        XFREE(kek, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        XFREE(encryptedKey, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        return ret;
    }

    /* generate encrypted key: encrypt CEK with KEK */
    ret = wc_PKCS7_PwriKek_KeyWrap(pkcs7, kek, kekKeySz, pkcs7->cek,
                                   pkcs7->cekSz, encryptedKey, &encryptedKeySz,
                                   tmpIv, kekBlockSz, encryptOID);
    if (ret < 0) {
        XFREE(recip, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        XFREE(kek, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        XFREE(encryptedKey, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        return ret;
    }
    encryptedKeySz = ret;

    /* put together encrypted key OCTET STRING */
    encKeyOctetStrSz = SetOctetString(encryptedKeySz, encKeyOctetStr);
    totalSz += (encKeyOctetStrSz + encryptedKeySz);

    /* put together IV OCTET STRING */
    ivOctetStringSz = SetOctetString(kekBlockSz, ivOctetString);
    totalSz += (ivOctetStringSz + kekBlockSz);

    /* set PWRIAlgorithms AlgorithmIdentifier, adding (ivOctetStringSz +
       blockKeySz) for IV OCTET STRING */
    pwriEncAlgoIdSz = SetAlgoID(encryptOID, pwriEncAlgoId,
                                oidBlkType, ivOctetStringSz + kekBlockSz);
    totalSz += pwriEncAlgoIdSz;

    /* set KeyEncryptionAlgorithms OID */
    ret = wc_SetContentType(PWRI_KEK_WRAP, keyEncAlgoId, sizeof(keyEncAlgoId));
    if (ret <= 0) {
        XFREE(recip, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        XFREE(kek, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        XFREE(encryptedKey, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        return ret;
    }
    keyEncAlgoIdSz = ret;
    totalSz += keyEncAlgoIdSz;

    /* KeyEncryptionAlgorithm SEQ */
    keyEncAlgoIdSeqSz = SetSequence(keyEncAlgoIdSz + pwriEncAlgoIdSz +
                                    ivOctetStringSz + kekBlockSz,
                                    keyEncAlgoIdSeq);
    totalSz += keyEncAlgoIdSeqSz;

    /* set KDF salt */
    kdfSaltOctetStrSz = SetOctetString(saltSz, kdfSaltOctetStr);
    totalSz += (kdfSaltOctetStrSz + saltSz);

    /* set KDF iteration count */
    kdfIterationsSz = SetMyVersion(iterations, kdfIterations, 0);
    totalSz += kdfIterationsSz;

    /* set KDF params SEQ */
    kdfParamsSeqSz = SetSequence(kdfSaltOctetStrSz + saltSz + kdfIterationsSz,
                                 kdfParamsSeq);
    totalSz += kdfParamsSeqSz;

    /* set KDF algo OID */
    ret = wc_SetContentType(kdfOID, kdfAlgoId, sizeof(kdfAlgoId));
    if (ret <= 0) {
        XFREE(recip, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        XFREE(kek, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        XFREE(encryptedKey, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        return ret;
    }
    kdfAlgoIdSz = ret;
    totalSz += kdfAlgoIdSz;

    /* set KeyDerivationAlgorithmIdentifier EXPLICIT [0] SEQ */
    kdfAlgoIdSeqSz = SetExplicit(0, kdfAlgoIdSz + kdfParamsSeqSz +
                                 kdfSaltOctetStrSz + saltSz + kdfIterationsSz,
                                 kdfAlgoIdSeq);
    totalSz += kdfAlgoIdSeqSz;

    /* set PasswordRecipientInfo CMSVersion, MUST be 0 */
    verSz = SetMyVersion(0, ver, 0);
    totalSz += verSz;
    recip->recipVersion = 0;

    /* set PasswordRecipientInfo SEQ */
    recipSeqSz = SetImplicit(ASN_SEQUENCE, 3, totalSz, recipSeq);
    totalSz += recipSeqSz;

    if (totalSz > MAX_RECIP_SZ) {
        WOLFSSL_MSG("CMS Recipient output buffer too small");
        XFREE(recip, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        XFREE(kek, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        XFREE(encryptedKey, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        return BUFFER_E;
    }

    idx = 0;
    XMEMCPY(recip->recip + idx, recipSeq, recipSeqSz);
    idx += recipSeqSz;
    XMEMCPY(recip->recip + idx, ver, verSz);
    idx += verSz;
    XMEMCPY(recip->recip + idx, kdfAlgoIdSeq, kdfAlgoIdSeqSz);
    idx += kdfAlgoIdSeqSz;
    XMEMCPY(recip->recip + idx, kdfAlgoId, kdfAlgoIdSz);
    idx += kdfAlgoIdSz;
    XMEMCPY(recip->recip + idx, kdfParamsSeq, kdfParamsSeqSz);
    idx += kdfParamsSeqSz;
    XMEMCPY(recip->recip + idx, kdfSaltOctetStr, kdfSaltOctetStrSz);
    idx += kdfSaltOctetStrSz;
    XMEMCPY(recip->recip + idx, salt, saltSz);
    idx += saltSz;
    XMEMCPY(recip->recip + idx, kdfIterations, kdfIterationsSz);
    idx += kdfIterationsSz;
    XMEMCPY(recip->recip + idx, keyEncAlgoIdSeq, keyEncAlgoIdSeqSz);
    idx += keyEncAlgoIdSeqSz;
    XMEMCPY(recip->recip + idx, keyEncAlgoId, keyEncAlgoIdSz);
    idx += keyEncAlgoIdSz;
    XMEMCPY(recip->recip + idx, pwriEncAlgoId, pwriEncAlgoIdSz);
    idx += pwriEncAlgoIdSz;
    XMEMCPY(recip->recip + idx, ivOctetString, ivOctetStringSz);
    idx += ivOctetStringSz;
    XMEMCPY(recip->recip + idx, tmpIv, kekBlockSz);
    idx += kekBlockSz;
    XMEMCPY(recip->recip + idx, encKeyOctetStr, encKeyOctetStrSz);
    idx += encKeyOctetStrSz;
    XMEMCPY(recip->recip + idx, encryptedKey, encryptedKeySz);
    idx += encryptedKeySz;

    ForceZero(kek, kekBlockSz);
    ForceZero(encryptedKey, encryptedKeySz);
    XFREE(kek, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
    XFREE(encryptedKey, pkcs7->heap, DYNAMIC_TYPE_PKCS7);

    /* store recipient size */
    recip->recipSz = idx;
    recip->recipType = PKCS7_PWRI;

    /* add recipient to recip list */
    if (pkcs7->recipList == NULL) {
        pkcs7->recipList = recip;
    } else {
        lastRecip = pkcs7->recipList;
        while (lastRecip->next != NULL) {
            lastRecip = lastRecip->next;
        }
        lastRecip->next = recip;
    }

    (void)options;

    return idx;
}

/* Import password and KDF settings into a PKCS7 structure. Used for setting
 * the password info for decryption a EnvelopedData PWRI RecipientInfo.
 *
 * Returns 0 on success, negative upon error */
int wc_PKCS7_SetPassword(PKCS7* pkcs7, byte* passwd, word32 pLen)
{
    if (pkcs7 == NULL || passwd == NULL || pLen == 0)
        return BAD_FUNC_ARG;

    pkcs7->pass = passwd;
    pkcs7->passSz = pLen;

    return 0;
}

#endif /* NO_PWDBASED */


/* Encode and add CMS EnvelopedData KEKRI (KEKRecipientInfo) RecipientInfo
 * to CMS/PKCS#7 EnvelopedData structure.
 *
 * pkcs7 - pointer to initialized PKCS7 structure
 * keyWrapOID - OID sum of key wrap algorithm identifier
 * kek        - key encryption key
 * kekSz      - size of kek, bytes
 * keyID      - key-encryption key identifier, pre-distributed to endpoints
 * keyIDSz    - size of keyID, bytes
 * timePtr    - pointer to "time_t", which is typically "long" (OPTIONAL)
 * otherOID   - ASN.1 encoded OID of other attribute (OPTIONAL)
 * otherOIDSz - size of otherOID, bytes (OPTIONAL)
 * other      - other attribute (OPTIONAL)
 * otherSz    - size of other (OPTIONAL)
 *
 * Returns 0 on success, negative upon error */
int wc_PKCS7_AddRecipient_KEKRI(PKCS7* pkcs7, int keyWrapOID, byte* kek,
                                word32 kekSz, byte* keyId, word32 keyIdSz,
                                void* timePtr, byte* otherOID,
                                word32 otherOIDSz, byte* other, word32 otherSz,
                                int options)
{
    Pkcs7EncodedRecip* recip = NULL;
    Pkcs7EncodedRecip* lastRecip = NULL;

    byte recipSeq[MAX_SEQ_SZ];
    byte ver[MAX_VERSION_SZ];
    byte kekIdSeq[MAX_SEQ_SZ];
    byte kekIdOctetStr[MAX_OCTET_STR_SZ];
    byte genTime[ASN_GENERALIZED_TIME_SIZE];
    byte otherAttSeq[MAX_SEQ_SZ];
    byte encAlgoId[MAX_ALGO_SZ];
    byte encKeyOctetStr[MAX_OCTET_STR_SZ];
#ifdef WOLFSSL_SMALL_STACK
    byte* encryptedKey;
#else
    byte encryptedKey[MAX_ENCRYPTED_KEY_SZ];
#endif

    int blockKeySz = 0, ret = 0;
    word32 idx = 0;
    word32 totalSz = 0;
    word32 recipSeqSz = 0, verSz = 0;
    word32 kekIdSeqSz = 0, kekIdOctetStrSz = 0;
    word32 otherAttSeqSz = 0, encAlgoIdSz = 0, encKeyOctetStrSz = 0;
    word32 encryptedKeySz;

    int timeSz = 0;
    time_t* tm = NULL;

    if (pkcs7 == NULL || kek == NULL || keyId == NULL)
        return BAD_FUNC_ARG;

    recip = (Pkcs7EncodedRecip*)XMALLOC(sizeof(Pkcs7EncodedRecip), pkcs7->heap,
                                 DYNAMIC_TYPE_PKCS7);
    if (recip == NULL)
        return MEMORY_E;

    XMEMSET(recip, 0, sizeof(Pkcs7EncodedRecip));

    /* get key size for content-encryption key based on algorithm */
    blockKeySz = wc_PKCS7_GetOIDKeySize(pkcs7->encryptOID);
    if (blockKeySz < 0) {
        XFREE(recip, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        return blockKeySz;
    }

    /* generate random content encryption key, if needed */
    ret = PKCS7_GenerateContentEncryptionKey(pkcs7, blockKeySz);
    if (ret < 0) {
        XFREE(recip, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        return ret;
    }

    /* EncryptedKey */
#ifdef WOLFSSL_SMALL_STACK
    encryptedKey = (byte*)XMALLOC(MAX_ENCRYPTED_KEY_SZ, pkcs7->heap,
                                  DYNAMIC_TYPE_PKCS7);
    if (encryptedKey == NULL) {
        XFREE(recip, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        return MEMORY_E;
    }
#endif
    encryptedKeySz = MAX_ENCRYPTED_KEY_SZ;
    XMEMSET(encryptedKey, 0, encryptedKeySz);

    encryptedKeySz = wc_PKCS7_KeyWrap(pkcs7->cek, pkcs7->cekSz, kek, kekSz,
                                      encryptedKey, encryptedKeySz, keyWrapOID,
                                      AES_ENCRYPTION);
    if (encryptedKeySz <= 0) {
    #ifdef WOLFSSL_SMALL_STACK
        XFREE(encryptedKey, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
    #endif
        XFREE(recip, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        return encryptedKeySz;
    }

    encKeyOctetStrSz = SetOctetString(encryptedKeySz, encKeyOctetStr);
    totalSz += (encKeyOctetStrSz + encryptedKeySz);

    /* KeyEncryptionAlgorithmIdentifier */
    encAlgoIdSz = SetAlgoID(keyWrapOID, encAlgoId, oidKeyWrapType, 0);
    totalSz += encAlgoIdSz;

    /* KEKIdentifier: keyIdentifier */
    kekIdOctetStrSz = SetOctetString(keyIdSz, kekIdOctetStr);
    totalSz += (kekIdOctetStrSz + keyIdSz);

    /* KEKIdentifier: GeneralizedTime (OPTIONAL) */
    if (timePtr != NULL) {
        tm = (time_t*)timePtr;
        timeSz = GetAsnTimeString(tm, genTime, sizeof(genTime));
        if (timeSz < 0) {
            XFREE(recip, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        #ifdef WOLFSSL_SMALL_STACK
            XFREE(encryptedKey, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        #endif
            return timeSz;
        }
        totalSz += timeSz;
    }

    /* KEKIdentifier: OtherKeyAttribute SEQ (OPTIONAL) */
    if (other != NULL && otherSz > 0) {
        otherAttSeqSz = SetSequence(otherOIDSz + otherSz, otherAttSeq);
        totalSz += otherAttSeqSz + otherOIDSz + otherSz;
    }

    /* KEKIdentifier SEQ */
    kekIdSeqSz = SetSequence(kekIdOctetStrSz + keyIdSz + timeSz +
                             otherAttSeqSz + otherOIDSz + otherSz, kekIdSeq);
    totalSz += kekIdSeqSz;

    /* version */
    verSz = SetMyVersion(4, ver, 0);
    totalSz += verSz;
    recip->recipVersion = 4;

    /* KEKRecipientInfo SEQ */
    recipSeqSz = SetImplicit(ASN_SEQUENCE, 2, totalSz, recipSeq);
    totalSz += recipSeqSz;

    if (totalSz > MAX_RECIP_SZ) {
        WOLFSSL_MSG("CMS Recipient output buffer too small");
        XFREE(recip, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
    #ifdef WOLFSSL_SMALL_STACK
        XFREE(encryptedKey, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
    #endif
        return BUFFER_E;
    }

    XMEMCPY(recip->recip + idx, recipSeq, recipSeqSz);
    idx += recipSeqSz;
    XMEMCPY(recip->recip + idx, ver, verSz);
    idx += verSz;
    XMEMCPY(recip->recip + idx, kekIdSeq, kekIdSeqSz);
    idx += kekIdSeqSz;
    XMEMCPY(recip->recip + idx, kekIdOctetStr, kekIdOctetStrSz);
    idx += kekIdOctetStrSz;
    XMEMCPY(recip->recip + idx, keyId, keyIdSz);
    idx += keyIdSz;
    if (timePtr != NULL) {
        XMEMCPY(recip->recip + idx, genTime, timeSz);
        idx += timeSz;
    }
    if (other != NULL && otherSz > 0) {
        XMEMCPY(recip->recip + idx, otherAttSeq, otherAttSeqSz);
        idx += otherAttSeqSz;
        XMEMCPY(recip->recip + idx, otherOID, otherOIDSz);
        idx += otherOIDSz;
        XMEMCPY(recip->recip + idx, other, otherSz);
        idx += otherSz;
    }
    XMEMCPY(recip->recip + idx, encAlgoId, encAlgoIdSz);
    idx += encAlgoIdSz;
    XMEMCPY(recip->recip + idx, encKeyOctetStr, encKeyOctetStrSz);
    idx += encKeyOctetStrSz;
    XMEMCPY(recip->recip + idx, encryptedKey, encryptedKeySz);
    idx += encryptedKeySz;

#ifdef WOLFSSL_SMALL_STACK
    XFREE(encryptedKey, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
#endif

    /* store recipient size */
    recip->recipSz = idx;
    recip->recipType = PKCS7_KEKRI;

    /* add recipient to recip list */
    if (pkcs7->recipList == NULL) {
        pkcs7->recipList = recip;
    } else {
        lastRecip = pkcs7->recipList;
        while(lastRecip->next != NULL) {
            lastRecip = lastRecip->next;
        }
        lastRecip->next = recip;
    }

    (void)options;

    return idx;
}


static int wc_PKCS7_GetCMSVersion(PKCS7* pkcs7, int cmsContentType)
{
    int version = -1;

    if (pkcs7 == NULL)
        return BAD_FUNC_ARG;

    switch (cmsContentType) {
        case ENVELOPED_DATA:

            /* NOTE: EnvelopedData does not currently support
               originatorInfo or unprotectedAttributes. When either of these
               are added, version checking below needs to be updated to match
               Section 6.1 of RFC 5652 */

            /* if RecipientInfos include pwri or ori, version is 3 */
            if (wc_PKCS7_RecipientListIncludesType(pkcs7, PKCS7_PWRI) ||
                wc_PKCS7_RecipientListIncludesType(pkcs7, PKCS7_ORI)) {
                version = 3;
                break;
            }

            /* if unprotectedAttrs is absent AND all RecipientInfo structs
               are version 0, version is 0 */
            if (wc_PKCS7_RecipientListVersionsAllZero(pkcs7)) {
                version = 0;
                break;
            }

            /* otherwise, version is 2 */
            version = 2;
            break;

        default:
            break;
    }

    return version;
}


/* build PKCS#7 envelopedData content type, return enveloped size */
int wc_PKCS7_EncodeEnvelopedData(PKCS7* pkcs7, byte* output, word32 outputSz)
{
    int ret, idx = 0;
    int totalSz, padSz, encryptedOutSz;

    int contentInfoSeqSz, outerContentTypeSz, outerContentSz;
    byte contentInfoSeq[MAX_SEQ_SZ];
    byte outerContentType[MAX_ALGO_SZ];
    byte outerContent[MAX_SEQ_SZ];

    int kariVersion;
    int envDataSeqSz, verSz;
    byte envDataSeq[MAX_SEQ_SZ];
    byte ver[MAX_VERSION_SZ];

    WC_RNG rng;
    int blockSz, blockKeySz;
    byte* plain;
    byte* encryptedContent;

    Pkcs7EncodedRecip* tmpRecip = NULL;
    int recipSz, recipSetSz;
    byte recipSet[MAX_SET_SZ];

    int encContentOctetSz, encContentSeqSz, contentTypeSz;
    int contentEncAlgoSz, ivOctetStringSz;
    byte encContentSeq[MAX_SEQ_SZ];
    byte contentType[MAX_ALGO_SZ];
    byte contentEncAlgo[MAX_ALGO_SZ];
    byte tmpIv[MAX_CONTENT_IV_SIZE];
    byte ivOctetString[MAX_OCTET_STR_SZ];
    byte encContentOctet[MAX_OCTET_STR_SZ];

    if (pkcs7 == NULL || pkcs7->content == NULL || pkcs7->contentSz == 0)
        return BAD_FUNC_ARG;

    if (output == NULL || outputSz == 0)
        return BAD_FUNC_ARG;

    blockKeySz = wc_PKCS7_GetOIDKeySize(pkcs7->encryptOID);
    if (blockKeySz < 0)
        return blockKeySz;

    blockSz = wc_PKCS7_GetOIDBlockSize(pkcs7->encryptOID);
    if (blockSz < 0)
        return blockSz;

    /* outer content type */
    ret = wc_SetContentType(ENVELOPED_DATA, outerContentType,
                            sizeof(outerContentType));
    if (ret < 0)
        return ret;

    outerContentTypeSz = ret;

    /* generate random content encryption key */
    ret = PKCS7_GenerateContentEncryptionKey(pkcs7, blockKeySz);
    if (ret != 0) {
        return ret;
    }

    /* build RecipientInfo, only if user manually set singleCert and size */
    if (pkcs7->singleCert != NULL && pkcs7->singleCertSz > 0) {
        switch (pkcs7->publicKeyOID) {
        #ifndef NO_RSA
            case RSAk:
                ret = wc_PKCS7_AddRecipient_KTRI(pkcs7, pkcs7->singleCert,
                                                 pkcs7->singleCertSz, 0);
                break;
        #endif
        #ifdef HAVE_ECC
            case ECDSAk:
                ret = wc_PKCS7_AddRecipient_KARI(pkcs7, pkcs7->singleCert,
                                                 pkcs7->singleCertSz,
                                                 pkcs7->keyWrapOID,
                                                 pkcs7->keyAgreeOID, pkcs7->ukm,
                                                 pkcs7->ukmSz, 0);
                break;
        #endif

            default:
                WOLFSSL_MSG("Unsupported RecipientInfo public key type");
                return BAD_FUNC_ARG;
        };

        if (ret < 0) {
            WOLFSSL_MSG("Failed to create RecipientInfo");
            return ret;
        }
    }

    recipSz = wc_PKCS7_GetRecipientListSize(pkcs7);
    if (recipSz < 0) {
        return ret;

    } else if (recipSz == 0) {
        WOLFSSL_MSG("You must add at least one CMS recipient");
        return PKCS7_RECIP_E;
    }
    recipSetSz = SetSet(recipSz, recipSet);

    /* version, defined in Section 6.1 of RFC 5652 */
    kariVersion = wc_PKCS7_GetCMSVersion(pkcs7, ENVELOPED_DATA);
    if (kariVersion < 0) {
        WOLFSSL_MSG("Failed to set CMS EnvelopedData version");
        return PKCS7_RECIP_E;
    }

    verSz = SetMyVersion(kariVersion, ver, 0);

    ret = wc_InitRng_ex(&rng, pkcs7->heap, pkcs7->devId);
    if (ret != 0)
        return ret;

    /* generate IV for block cipher */
    ret = wc_PKCS7_GenerateBlock(pkcs7, &rng, tmpIv, blockSz);
    wc_FreeRng(&rng);
    if (ret != 0)
        return ret;

    /* EncryptedContentInfo */
    ret = wc_SetContentType(pkcs7->contentOID, contentType,
                            sizeof(contentType));
    if (ret < 0)
        return ret;

    contentTypeSz = ret;

    /* allocate encrypted content buffer and PKCS#7 padding */
    padSz = wc_PKCS7_GetPadSize(pkcs7->contentSz, blockSz);
    if (padSz < 0)
        return padSz;

    encryptedOutSz = pkcs7->contentSz + padSz;

    plain = (byte*)XMALLOC(encryptedOutSz, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
    if (plain == NULL)
        return MEMORY_E;

    ret = wc_PKCS7_PadData(pkcs7->content, pkcs7->contentSz, plain,
                           encryptedOutSz, blockSz);
    if (ret < 0) {
        XFREE(plain, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        return ret;
    }

    encryptedContent = (byte*)XMALLOC(encryptedOutSz, pkcs7->heap,
                                      DYNAMIC_TYPE_PKCS7);
    if (encryptedContent == NULL) {
        XFREE(plain, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        return MEMORY_E;
    }

    /* put together IV OCTET STRING */
    ivOctetStringSz = SetOctetString(blockSz, ivOctetString);

    /* build up our ContentEncryptionAlgorithmIdentifier sequence,
     * adding (ivOctetStringSz + blockSz) for IV OCTET STRING */
    contentEncAlgoSz = SetAlgoID(pkcs7->encryptOID, contentEncAlgo,
                                 oidBlkType, ivOctetStringSz + blockSz);

    if (contentEncAlgoSz == 0) {
        XFREE(encryptedContent, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        XFREE(plain, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        return BAD_FUNC_ARG;
    }

    /* encrypt content */
    ret = wc_PKCS7_EncryptContent(pkcs7->encryptOID, pkcs7->cek,
            pkcs7->cekSz, tmpIv, blockSz, NULL, 0, NULL, 0, plain,
            encryptedOutSz, encryptedContent);

    if (ret != 0) {
        XFREE(encryptedContent, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        XFREE(plain, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        return ret;
    }

    encContentOctetSz = SetImplicit(ASN_OCTET_STRING, 0, encryptedOutSz,
                                    encContentOctet);

    encContentSeqSz = SetSequence(contentTypeSz + contentEncAlgoSz +
                                  ivOctetStringSz + blockSz +
                                  encContentOctetSz + encryptedOutSz,
                                  encContentSeq);

    /* keep track of sizes for outer wrapper layering */
    totalSz = verSz + recipSetSz + recipSz + encContentSeqSz + contentTypeSz +
              contentEncAlgoSz + ivOctetStringSz + blockSz +
              encContentOctetSz + encryptedOutSz;

    /* EnvelopedData */
    envDataSeqSz = SetSequence(totalSz, envDataSeq);
    totalSz += envDataSeqSz;

    /* outer content */
    outerContentSz = SetExplicit(0, totalSz, outerContent);
    totalSz += outerContentTypeSz;
    totalSz += outerContentSz;

    /* ContentInfo */
    contentInfoSeqSz = SetSequence(totalSz, contentInfoSeq);
    totalSz += contentInfoSeqSz;

    if (totalSz > (int)outputSz) {
        WOLFSSL_MSG("Pkcs7_encrypt output buffer too small");
        XFREE(encryptedContent, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        XFREE(plain, pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
        return BUFFER_E;
    }

    XMEMCPY(output + idx, contentInfoSeq, contentInfoSeqSz);
    idx += contentInfoSeqSz;
    XMEMCPY(output + idx, outerContentType, outerContentTypeSz);
    idx += outerContentTypeSz;
    XMEMCPY(output + idx, outerContent, outerContentSz);
    idx += outerContentSz;
    XMEMCPY(output + idx, envDataSeq, envDataSeqSz);
    idx += envDataSeqSz;
    XMEMCPY(output + idx, ver, verSz);
    idx += verSz;
    XMEMCPY(output + idx, recipSet, recipSetSz);
    idx += recipSetSz;
    /* copy in recipients from list */
    tmpRecip = pkcs7->recipList;
    while (tmpRecip != NULL) {
        XMEMCPY(output + idx, tmpRecip->recip, tmpRecip->recipSz);
        idx += tmpRecip->recipSz;
        tmpRecip = tmpRecip->next;
    }
    wc_PKCS7_FreeEncodedRecipientSet(pkcs7);
    XMEMCPY(output + idx, encContentSeq, encContentSeqSz);
    idx += encContentSeqSz;
    XMEMCPY(output + idx, contentType, contentTypeSz);
    idx += contentTypeSz;
    XMEMCPY(output + idx, contentEncAlgo, contentEncAlgoSz);
    idx += contentEncAlgoSz;
    XMEMCPY(output + idx, ivOctetString, ivOctetStringSz);
    idx += ivOctetStringSz;
    XMEMCPY(output + idx, tmpIv, blockSz);
    idx += blockSz;
    XMEMCPY(output + idx, encContentOctet, encContentOctetSz);
    idx += encContentOctetSz;
    XMEMCPY(output + idx, encryptedContent, encryptedOutSz);
    idx += encryptedOutSz;

    XFREE(plain, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
    XFREE(encryptedContent, pkcs7->heap, DYNAMIC_TYPE_PKCS7);

    return idx;
}

#ifndef NO_RSA
/* decode KeyTransRecipientInfo (ktri), return 0 on success, <0 on error */
static int wc_PKCS7_DecryptKtri(PKCS7* pkcs7, byte* in, word32 inSz,
                               word32* idx, byte* decryptedKey,
                               word32* decryptedKeySz, int* recipFound)
{
    int length, encryptedKeySz = 0, ret = 0;
    int keySz, version, sidType = 0;
    word32 encOID;
    word32 keyIdx;
    byte   issuerHash[KEYID_SIZE];
    byte*  outKey   = NULL;
    byte* pkiMsg    = in;
    word32 pkiMsgSz = inSz;

#ifndef NO_PKCS7_STREAM
    word32 tmpIdx = *idx;
#endif
#ifdef WC_RSA_BLINDING
    WC_RNG rng;
#endif

#ifdef WOLFSSL_SMALL_STACK
    mp_int* serialNum  = NULL;
    byte* encryptedKey = NULL;
    RsaKey* privKey    = NULL;
#else
    mp_int serialNum[1];
    byte encryptedKey[MAX_ENCRYPTED_KEY_SZ];
    RsaKey privKey[1];
#endif

    switch (pkcs7->state) {
        case WC_PKCS7_DECRYPT_KTRI:
        #ifndef NO_PKCS7_STREAM
            if ((ret = wc_PKCS7_AddDataToStream(pkcs7, in, inSz, MAX_VERSION_SZ,
                            &pkiMsg, idx)) != 0) {
                return ret;
            }

            pkiMsgSz = wc_PKCS7_GetMaxStream(pkcs7, PKCS7_DEFAULT_PEEK, in, inSz);

        #endif
            if (GetMyVersion(pkiMsg, idx, &version, pkiMsgSz) < 0)
                return ASN_PARSE_E;

            if (version == 0) {
                sidType = CMS_ISSUER_AND_SERIAL_NUMBER;
            } else if (version == 2) {
                sidType = CMS_SKID;
            } else {
                return ASN_VERSION_E;
            }

        #ifndef NO_PKCS7_STREAM
            if ((ret = wc_PKCS7_StreamEndCase(pkcs7, &tmpIdx, idx)) != 0) {
                    break;
            }
            wc_PKCS7_StreamStoreVar(pkcs7, 0, sidType, version);

            /* @TODO getting total amount left because of GetInt call later on
             * this could be optimized to stream better */
            pkcs7->stream->expected = (pkcs7->stream->maxLen -
                                pkcs7->stream->totalRd) + pkcs7->stream->length;
        #endif
            wc_PKCS7_ChangeState(pkcs7, WC_PKCS7_DECRYPT_KTRI_2);
            FALL_THROUGH;

        case WC_PKCS7_DECRYPT_KTRI_2:
        #ifndef NO_PKCS7_STREAM

            if ((ret = wc_PKCS7_AddDataToStream(pkcs7, in, inSz, pkcs7->stream->expected,
                            &pkiMsg, idx)) != 0) {
                return ret;
            }

            pkiMsgSz = wc_PKCS7_GetMaxStream(pkcs7, PKCS7_DEFAULT_PEEK, in, inSz);
            wc_PKCS7_StreamGetVar(pkcs7, NULL, &sidType, &version);

            /* @TODO get expected size for next part, does not account for
             * GetInt call well */
            if (pkcs7->stream->expected == MAX_SEQ_SZ) {
                int sz;
                word32 lidx;

                if (sidType == CMS_ISSUER_AND_SERIAL_NUMBER) {
                    lidx = *idx;
                    ret = GetSequence(pkiMsg, &lidx, &sz, pkiMsgSz);
                    if (ret < 0)
                        return ret;
                }
                else {
                    lidx = *idx + ASN_TAG_SZ;
                    ret = GetLength(pkiMsg, &lidx, &sz, pkiMsgSz);
                    if (ret < 0)
                        return ret;
                }

                pkcs7->stream->expected = sz + MAX_ALGO_SZ + ASN_TAG_SZ +
                                          MAX_LENGTH_SZ;
                if (pkcs7->stream->length > 0 &&
                        pkcs7->stream->length < pkcs7->stream->expected) {
                    return WC_PKCS7_WANT_READ_E;
                }
            }
        #endif

            if (sidType == CMS_ISSUER_AND_SERIAL_NUMBER) {

                /* remove IssuerAndSerialNumber */
                if (GetSequence(pkiMsg, idx, &length, pkiMsgSz) < 0)
                    return ASN_PARSE_E;

                if (GetNameHash(pkiMsg, idx, issuerHash, pkiMsgSz) < 0)
                    return ASN_PARSE_E;

                /* if we found correct recipient, issuer hashes will match */
                if (XMEMCMP(issuerHash, pkcs7->issuerHash, KEYID_SIZE) == 0) {
                    *recipFound = 1;
                }

        #ifdef WOLFSSL_SMALL_STACK
                serialNum = (mp_int*)XMALLOC(sizeof(mp_int), pkcs7->heap,
                                             DYNAMIC_TYPE_TMP_BUFFER);
                if (serialNum == NULL)
                    return MEMORY_E;
        #endif

                if (GetInt(serialNum, pkiMsg, idx, pkiMsgSz) < 0) {
        #ifdef WOLFSSL_SMALL_STACK
                    XFREE(serialNum, pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
        #endif
                    return ASN_PARSE_E;
                }

                mp_clear(serialNum);

        #ifdef WOLFSSL_SMALL_STACK
                XFREE(serialNum, pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
        #endif

            } else {

                /* remove SubjectKeyIdentifier */
                if (pkiMsg[(*idx)++] !=
                        (ASN_CONSTRUCTED | ASN_CONTEXT_SPECIFIC | 0))
                    return ASN_PARSE_E;

                if (GetLength(pkiMsg, idx, &length, pkiMsgSz) < 0)
                    return ASN_PARSE_E;

                if (pkiMsg[(*idx)++] != ASN_OCTET_STRING)
                    return ASN_PARSE_E;

                if (GetLength(pkiMsg, idx, &length, pkiMsgSz) < 0)
                    return ASN_PARSE_E;

                /* if we found correct recipient, SKID will match */
                if (XMEMCMP(pkiMsg + (*idx), pkcs7->issuerSubjKeyId,
                            KEYID_SIZE) == 0) {
                    *recipFound = 1;
                }
                (*idx) += KEYID_SIZE;
            }

            if (GetAlgoId(pkiMsg, idx, &encOID, oidKeyType, pkiMsgSz) < 0)
                return ASN_PARSE_E;

            /* key encryption algorithm must be RSA for now */
            if (encOID != RSAk)
                return ALGO_ID_E;

            /* read encryptedKey */
            if (pkiMsg[(*idx)++] != ASN_OCTET_STRING) {
                return ASN_PARSE_E;
            }

            if (GetLength(pkiMsg, idx, &encryptedKeySz, pkiMsgSz) < 0) {
                return ASN_PARSE_E;
            }
            if (encryptedKeySz > MAX_ENCRYPTED_KEY_SZ) {
               return BUFFER_E;
            }

        #ifndef NO_PKCS7_STREAM
            if ((ret = wc_PKCS7_StreamEndCase(pkcs7, &tmpIdx, idx)) != 0) {
                    break;
            }
            wc_PKCS7_StreamStoreVar(pkcs7, encryptedKeySz, sidType, version);
            pkcs7->stream->expected = encryptedKeySz;
        #endif
            wc_PKCS7_ChangeState(pkcs7, WC_PKCS7_DECRYPT_KTRI_3);
            FALL_THROUGH;

        case WC_PKCS7_DECRYPT_KTRI_3:
        #ifndef NO_PKCS7_STREAM
            if ((ret = wc_PKCS7_AddDataToStream(pkcs7, in, inSz,
                            pkcs7->stream->expected, &pkiMsg, idx)) != 0) {
                return ret;
            }
            encryptedKeySz = pkcs7->stream->expected;
        #endif

        #ifdef WOLFSSL_SMALL_STACK
            encryptedKey = (byte*)XMALLOC(encryptedKeySz, pkcs7->heap,
                                          DYNAMIC_TYPE_TMP_BUFFER);
            if (encryptedKey == NULL)
                return MEMORY_E;
        #endif

            if (*recipFound == 1)
                XMEMCPY(encryptedKey, &pkiMsg[*idx], encryptedKeySz);
            *idx += encryptedKeySz;

            /* load private key */
        #ifdef WOLFSSL_SMALL_STACK
            privKey = (RsaKey*)XMALLOC(sizeof(RsaKey), pkcs7->heap,
                DYNAMIC_TYPE_TMP_BUFFER);
            if (privKey == NULL) {
                XFREE(encryptedKey, pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
                return MEMORY_E;
            }
        #endif

            ret = wc_InitRsaKey_ex(privKey, pkcs7->heap, INVALID_DEVID);
            if (ret != 0) {
        #ifdef WOLFSSL_SMALL_STACK
                XFREE(encryptedKey, pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
                XFREE(privKey, pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
        #endif
                return ret;
            }

            if (pkcs7->privateKey != NULL && pkcs7->privateKeySz > 0) {
                keyIdx = 0;
                ret = wc_RsaPrivateKeyDecode(pkcs7->privateKey, &keyIdx,
                        privKey, pkcs7->privateKeySz);
            }
            else if (pkcs7->devId == INVALID_DEVID) {
                ret = BAD_FUNC_ARG;
            }
            if (ret != 0) {
                WOLFSSL_MSG("Failed to decode RSA private key");
                wc_FreeRsaKey(privKey);
        #ifdef WOLFSSL_SMALL_STACK
                XFREE(encryptedKey, pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
                XFREE(privKey, pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
        #endif
                return ret;
            }

            /* decrypt encryptedKey */
            #ifdef WC_RSA_BLINDING
            ret = wc_InitRng_ex(&rng, pkcs7->heap, pkcs7->devId);
            if (ret == 0) {
                ret = wc_RsaSetRNG(privKey, &rng);
            }
            #endif
            if (ret == 0) {
                keySz = wc_RsaPrivateDecryptInline(encryptedKey, encryptedKeySz,
                                                   &outKey, privKey);
                #ifdef WC_RSA_BLINDING
                    wc_FreeRng(&rng);
                #endif
            } else {
                keySz = ret;
            }
            wc_FreeRsaKey(privKey);

            if (keySz <= 0 || outKey == NULL) {
                ForceZero(encryptedKey, encryptedKeySz);
        #ifdef WOLFSSL_SMALL_STACK
                XFREE(encryptedKey, pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
                XFREE(privKey, pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
        #endif
                return keySz;
            } else {
                *decryptedKeySz = keySz;
                XMEMCPY(decryptedKey, outKey, keySz);
                ForceZero(encryptedKey, encryptedKeySz);
            }

        #ifdef WOLFSSL_SMALL_STACK
            XFREE(encryptedKey, pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
            XFREE(privKey, pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
        #endif

        #ifndef NO_PKCS7_STREAM
            if ((ret = wc_PKCS7_StreamEndCase(pkcs7, &tmpIdx, idx)) != 0) {
                break;
            }
        #endif
            ret = 0; /* success */
            break;

        default:
            WOLFSSL_MSG("PKCS7 Unknown KTRI decrypt state");
            ret = BAD_FUNC_ARG;
    }

    return ret;
}
#endif /* !NO_RSA */

#ifdef HAVE_ECC

/* remove ASN.1 OriginatorIdentifierOrKey, return 0 on success, <0 on error */
static int wc_PKCS7_KariGetOriginatorIdentifierOrKey(WC_PKCS7_KARI* kari,
                        byte* pkiMsg, word32 pkiMsgSz, word32* idx)
{
    int ret, length;
    word32 keyOID;

    if (kari == NULL || pkiMsg == NULL || idx == NULL)
        return BAD_FUNC_ARG;

    /* remove OriginatorIdentifierOrKey */
    if (pkiMsg[*idx] == (ASN_CONSTRUCTED | ASN_CONTEXT_SPECIFIC | 0)) {
        (*idx)++;
        if (GetLength(pkiMsg, idx, &length, pkiMsgSz) < 0)
            return ASN_PARSE_E;

    } else {
        return ASN_PARSE_E;
    }

    /* remove OriginatorPublicKey */
    if (pkiMsg[*idx] == (ASN_CONSTRUCTED | ASN_CONTEXT_SPECIFIC | 1)) {
        (*idx)++;
        if (GetLength(pkiMsg, idx, &length, pkiMsgSz) < 0)
            return ASN_PARSE_E;

    } else {
        return ASN_PARSE_E;
    }

    /* remove AlgorithmIdentifier */
    if (GetAlgoId(pkiMsg, idx, &keyOID, oidKeyType, pkiMsgSz) < 0)
        return ASN_PARSE_E;

    if (keyOID != ECDSAk)
        return ASN_PARSE_E;

    /* remove ECPoint BIT STRING */
    if ((pkiMsgSz > (*idx + 1)) && (pkiMsg[(*idx)++] != ASN_BIT_STRING))
        return ASN_PARSE_E;

    if (GetLength(pkiMsg, idx, &length, pkiMsgSz) < 0)
        return ASN_PARSE_E;

    if ((pkiMsgSz < (*idx + 1)) || (pkiMsg[(*idx)++] != 0x00))
        return ASN_EXPECT_0_E;

    /* get sender ephemeral public ECDSA key */
    ret = wc_ecc_init_ex(kari->senderKey, kari->heap, kari->devId);
    if (ret != 0)
        return ret;

    kari->senderKeyInit = 1;

    /* length-1 for unused bits counter */
    ret = wc_ecc_import_x963(pkiMsg + (*idx), length - 1, kari->senderKey);
    if (ret != 0)
        return ret;

    (*idx) += length - 1;

    return 0;
}


/* remove optional UserKeyingMaterial if available, return 0 on success,
 * < 0 on error */
static int wc_PKCS7_KariGetUserKeyingMaterial(WC_PKCS7_KARI* kari,
                        byte* pkiMsg, word32 pkiMsgSz, word32* idx)
{
    int length;
    word32 savedIdx;

    if (kari == NULL || pkiMsg == NULL || idx == NULL)
        return BAD_FUNC_ARG;

    savedIdx = *idx;

    /* starts with EXPLICIT [1] */
    if (pkiMsg[(*idx)++] != (ASN_CONSTRUCTED | ASN_CONTEXT_SPECIFIC | 1)) {
        *idx = savedIdx;
        return 0;
    }

    if (GetLength(pkiMsg, idx, &length, pkiMsgSz) < 0) {
        *idx = savedIdx;
        return 0;
    }

    /* get OCTET STRING */
    if ( (pkiMsgSz > ((*idx) + 1)) &&
         (pkiMsg[(*idx)++] != ASN_OCTET_STRING) ) {
        *idx = savedIdx;
        return 0;
    }

    if (GetLength(pkiMsg, idx, &length, pkiMsgSz) < 0) {
        *idx = savedIdx;
        return 0;
    }

    kari->ukm = NULL;
    if (length > 0) {
        kari->ukm = (byte*)XMALLOC(length, kari->heap, DYNAMIC_TYPE_PKCS7);
        if (kari->ukm == NULL)
            return MEMORY_E;

        XMEMCPY(kari->ukm, pkiMsg + (*idx), length);
        kari->ukmOwner = 1;
    }

    (*idx) += length;
    kari->ukmSz = length;

    return 0;
}


/* remove ASN.1 KeyEncryptionAlgorithmIdentifier, return 0 on success,
 * < 0 on error */
static int wc_PKCS7_KariGetKeyEncryptionAlgorithmId(WC_PKCS7_KARI* kari,
                        byte* pkiMsg, word32 pkiMsgSz, word32* idx,
                        word32* keyAgreeOID, word32* keyWrapOID)
{
    if (kari == NULL || pkiMsg == NULL || idx == NULL ||
        keyAgreeOID == NULL || keyWrapOID == NULL)
        return BAD_FUNC_ARG;

    /* remove KeyEncryptionAlgorithmIdentifier */
    if (GetAlgoId(pkiMsg, idx, keyAgreeOID, oidCmsKeyAgreeType,
                  pkiMsgSz) < 0)
        return ASN_PARSE_E;

    /* remove KeyWrapAlgorithm, stored in parameter of KeyEncAlgoId */
    if (GetAlgoId(pkiMsg, idx, keyWrapOID, oidKeyWrapType, pkiMsgSz) < 0)
        return ASN_PARSE_E;

    return 0;
}


/* remove ASN.1 SubjectKeyIdentifier, return 0 on success, < 0 on error
 * if subject key ID matches, recipFound is set to 1 */
static int wc_PKCS7_KariGetSubjectKeyIdentifier(WC_PKCS7_KARI* kari,
                        byte* pkiMsg, word32 pkiMsgSz, word32* idx,
                        int* recipFound)
{
    int length;
    byte subjKeyId[KEYID_SIZE];

    if (kari == NULL || pkiMsg == NULL || idx == NULL || recipFound == NULL)
        return BAD_FUNC_ARG;

    /* remove RecipientKeyIdentifier IMPLICIT [0] */
    if ( (pkiMsgSz > (*idx + 1)) &&
         (pkiMsg[(*idx)++] == (ASN_CONSTRUCTED | ASN_CONTEXT_SPECIFIC | 0)) ) {

        if (GetLength(pkiMsg, idx, &length, pkiMsgSz) < 0)
            return ASN_PARSE_E;

    } else {
        return ASN_PARSE_E;
    }

    /* remove SubjectKeyIdentifier */
    if ( (pkiMsgSz > (*idx + 1)) &&
         (pkiMsg[(*idx)++] != ASN_OCTET_STRING) )
        return ASN_PARSE_E;

    if (GetLength(pkiMsg, idx, &length, pkiMsgSz) < 0)
        return ASN_PARSE_E;

    if (length != KEYID_SIZE)
        return ASN_PARSE_E;

    XMEMCPY(subjKeyId, pkiMsg + (*idx), KEYID_SIZE);
    (*idx) += length;

    /* subject key id should match if recipient found */
    if (XMEMCMP(subjKeyId, kari->decoded->extSubjKeyId, KEYID_SIZE) == 0) {
        *recipFound = 1;
    }

    return 0;
}


/* remove ASN.1 IssuerAndSerialNumber, return 0 on success, < 0 on error
 * if issuer and serial number match, recipFound is set to 1 */
static int wc_PKCS7_KariGetIssuerAndSerialNumber(WC_PKCS7_KARI* kari,
                        byte* pkiMsg, word32 pkiMsgSz, word32* idx,
                        int* recipFound)
{
    int length, ret;
    byte issuerHash[KEYID_SIZE];
#ifdef WOLFSSL_SMALL_STACK
    mp_int* serial;
    mp_int* recipSerial;
#else
    mp_int  serial[1];
    mp_int  recipSerial[1];
#endif

    /* remove IssuerAndSerialNumber */
    if (GetSequence(pkiMsg, idx, &length, pkiMsgSz) < 0)
        return ASN_PARSE_E;

    if (GetNameHash(pkiMsg, idx, issuerHash, pkiMsgSz) < 0)
        return ASN_PARSE_E;

    /* if we found correct recipient, issuer hashes will match */
    if (XMEMCMP(issuerHash, kari->decoded->issuerHash, KEYID_SIZE) == 0) {
        *recipFound = 1;
    }

#ifdef WOLFSSL_SMALL_STACK
    serial = (mp_int*)XMALLOC(sizeof(mp_int), kari->heap,
                              DYNAMIC_TYPE_TMP_BUFFER);
    if (serial == NULL)
        return MEMORY_E;

    recipSerial = (mp_int*)XMALLOC(sizeof(mp_int), kari->heap,
                                   DYNAMIC_TYPE_TMP_BUFFER);
    if (recipSerial == NULL) {
        XFREE(serial, kari->heap, DYNAMIC_TYPE_TMP_BUFFER);
        return MEMORY_E;
    }
#endif

    if (GetInt(serial, pkiMsg, idx, pkiMsgSz) < 0) {
#ifdef WOLFSSL_SMALL_STACK
        XFREE(serial,      kari->heap, DYNAMIC_TYPE_TMP_BUFFER);
        XFREE(recipSerial, kari->heap, DYNAMIC_TYPE_TMP_BUFFER);
#endif
        return ASN_PARSE_E;
    }

    ret = mp_read_unsigned_bin(recipSerial, kari->decoded->serial,
                             kari->decoded->serialSz);
    if (ret != MP_OKAY) {
        mp_clear(serial);
        WOLFSSL_MSG("Failed to parse CMS recipient serial number");
#ifdef WOLFSSL_SMALL_STACK
        XFREE(serial,      kari->heap, DYNAMIC_TYPE_TMP_BUFFER);
        XFREE(recipSerial, kari->heap, DYNAMIC_TYPE_TMP_BUFFER);
#endif
        return ret;
    }

    if (mp_cmp(recipSerial, serial) != MP_EQ) {
        mp_clear(serial);
        mp_clear(recipSerial);
        WOLFSSL_MSG("CMS serial number does not match recipient");
#ifdef WOLFSSL_SMALL_STACK
        XFREE(serial,      kari->heap, DYNAMIC_TYPE_TMP_BUFFER);
        XFREE(recipSerial, kari->heap, DYNAMIC_TYPE_TMP_BUFFER);
#endif
        return PKCS7_RECIP_E;
    }

    mp_clear(serial);
    mp_clear(recipSerial);

#ifdef WOLFSSL_SMALL_STACK
    XFREE(serial,      kari->heap, DYNAMIC_TYPE_TMP_BUFFER);
    XFREE(recipSerial, kari->heap, DYNAMIC_TYPE_TMP_BUFFER);
#endif

    return 0;
}


/* remove ASN.1 RecipientEncryptedKeys, return 0 on success, < 0 on error */
static int wc_PKCS7_KariGetRecipientEncryptedKeys(WC_PKCS7_KARI* kari,
                        byte* pkiMsg, word32 pkiMsgSz, word32* idx,
                        int* recipFound, byte* encryptedKey,
                        int* encryptedKeySz)
{
    int length;
    int ret = 0;

    if (kari == NULL || pkiMsg == NULL || idx == NULL ||
        recipFound == NULL || encryptedKey == NULL)
        return BAD_FUNC_ARG;

    /* remove RecipientEncryptedKeys */
    if (GetSequence(pkiMsg, idx, &length, pkiMsgSz) < 0)
        return ASN_PARSE_E;

    /* remove RecipientEncryptedKeys */
    if (GetSequence(pkiMsg, idx, &length, pkiMsgSz) < 0)
        return ASN_PARSE_E;

    /* KeyAgreeRecipientIdentifier is CHOICE of IssuerAndSerialNumber
     * or [0] IMMPLICIT RecipientKeyIdentifier */
    if ( (pkiMsgSz > (*idx + 1)) &&
         (pkiMsg[*idx] == (ASN_CONSTRUCTED | ASN_CONTEXT_SPECIFIC | 0)) ) {

        /* try to get RecipientKeyIdentifier */
        ret = wc_PKCS7_KariGetSubjectKeyIdentifier(kari, pkiMsg, pkiMsgSz,
                                                   idx, recipFound);
    } else {
        /* try to get IssuerAndSerialNumber */
        ret = wc_PKCS7_KariGetIssuerAndSerialNumber(kari, pkiMsg, pkiMsgSz,
                                                    idx, recipFound);
    }

    /* if we don't have either option, malformed CMS */
    if (ret != 0)
        return ret;

    /* remove EncryptedKey */
    if ( (pkiMsgSz > (*idx + 1)) &&
         (pkiMsg[(*idx)++] != ASN_OCTET_STRING) )
        return ASN_PARSE_E;

    if (GetLength(pkiMsg, idx, &length, pkiMsgSz) < 0)
        return ASN_PARSE_E;

    /* put encrypted CEK in decryptedKey buffer for now, decrypt later */
    if (length > *encryptedKeySz)
        return BUFFER_E;

    XMEMCPY(encryptedKey, pkiMsg + (*idx), length);
    *encryptedKeySz = length;
    (*idx) += length;

    return 0;
}

#endif /* HAVE_ECC */


int wc_PKCS7_SetOriEncryptCtx(PKCS7* pkcs7, void* ctx)
{
    if (pkcs7 == NULL)
        return BAD_FUNC_ARG;

    pkcs7->oriEncryptCtx = ctx;

    return 0;
}


int wc_PKCS7_SetOriDecryptCtx(PKCS7* pkcs7, void* ctx)
{

    if (pkcs7 == NULL)
        return BAD_FUNC_ARG;

    pkcs7->oriDecryptCtx = ctx;

    return 0;
}


int wc_PKCS7_SetOriDecryptCb(PKCS7* pkcs7, CallbackOriDecrypt cb)
{
    if (pkcs7 == NULL)
        return BAD_FUNC_ARG;

    pkcs7->oriDecryptCb = cb;

    return 0;
}


/* Decrypt ASN.1 OtherRecipientInfo (ori), as defined by:
 *
 *   OtherRecipientInfo ::= SEQUENCE {
 *     oriType OBJECT IDENTIFIER,
 *     oriValue ANY DEFINED BY oriType }
 *
 * pkcs7          - pointer to initialized PKCS7 structure
 * pkiMsg         - pointer to encoded CMS bundle
 * pkiMsgSz       - size of pkiMsg, bytes
 * idx            - [IN/OUT] pointer to index into pkiMsg
 * decryptedKey   - [OUT] output buf for decrypted content encryption key
 * decryptedKeySz - [IN/OUT] size of buffer, size of decrypted key
 * recipFound     - [OUT] 1 if recipient has been found, 0 if not
 *
 * Return 0 on success, negative upon error.
 */
static int wc_PKCS7_DecryptOri(PKCS7* pkcs7, byte* in, word32 inSz,
                               word32* idx, byte* decryptedKey,
                               word32* decryptedKeySz, int* recipFound)
{
    int ret, seqSz, oriOIDSz;
    word32 oriValueSz, tmpIdx;
    byte* oriValue;
    byte oriOID[MAX_OID_SZ];

    byte* pkiMsg    = in;
    word32 pkiMsgSz = inSz;
#ifndef NO_PKCS7_STREAM
    word32 stateIdx = *idx;
#endif

    if (pkcs7->oriDecryptCb == NULL) {
        WOLFSSL_MSG("You must register an ORI Decrypt callback");
        return BAD_FUNC_ARG;
    }

    switch (pkcs7->state) {

        case WC_PKCS7_DECRYPT_ORI:
        #ifndef NO_PKCS7_STREAM
            /* @TODO for now just get full buffer, needs divided up */
            if ((ret = wc_PKCS7_AddDataToStream(pkcs7, in, inSz,
                   (pkcs7->stream->maxLen - pkcs7->stream->totalRd) +
                   pkcs7->stream->length, &pkiMsg, idx)) != 0) {
                return ret;
            }

            pkiMsgSz = wc_PKCS7_GetMaxStream(pkcs7, PKCS7_DEFAULT_PEEK, in,
                    inSz);
        #endif
            /* get OtherRecipientInfo sequence length */
            if (GetLength(pkiMsg, idx, &seqSz, pkiMsgSz) < 0)
                return ASN_PARSE_E;

            tmpIdx = *idx;

            /* remove and store oriType OBJECT IDENTIFIER */
            if (GetASNObjectId(pkiMsg, idx, &oriOIDSz, pkiMsgSz) != 0)
                return ASN_PARSE_E;

            XMEMCPY(oriOID, pkiMsg + *idx, oriOIDSz);
            *idx += oriOIDSz;

            /* get oriValue, increment idx */
            oriValue = pkiMsg + *idx;
            oriValueSz = seqSz - (*idx - tmpIdx);
            *idx += oriValueSz;

            /* pass oriOID and oriValue to user callback, expect back
               decryptedKey and size */
            ret = pkcs7->oriDecryptCb(pkcs7, oriOID, (word32)oriOIDSz, oriValue,
                                      oriValueSz, decryptedKey, decryptedKeySz,
                                      pkcs7->oriDecryptCtx);

            if (ret != 0 || decryptedKey == NULL || *decryptedKeySz == 0) {
                /* decrypt operation failed */
                *recipFound = 0;
                return PKCS7_RECIP_E;
            }

            /* mark recipFound, since we only support one RecipientInfo for now */
            *recipFound = 1;

        #ifndef NO_PKCS7_STREAM
            if ((ret = wc_PKCS7_StreamEndCase(pkcs7, &stateIdx, idx)) != 0) {
                break;
            }
        #endif
            ret = 0; /* success */
            break;

        default:
            WOLFSSL_MSG("PKCS7 ORI unknown state");
            ret = BAD_FUNC_ARG;

    }

    return ret;
}

#ifndef NO_PWDBASED

/* decode ASN.1 PasswordRecipientInfo (pwri), return 0 on success,
 * < 0 on error */
static int wc_PKCS7_DecryptPwri(PKCS7* pkcs7, byte* in, word32 inSz,
                               word32* idx, byte* decryptedKey,
                               word32* decryptedKeySz, int* recipFound)
{
    byte* salt;
    byte* cek;
    byte* kek;

    byte tmpIv[MAX_CONTENT_IV_SIZE];

    int ret = 0, length, saltSz, iterations, blockSz, kekKeySz;
    int hashOID = WC_SHA; /* default to SHA1 */
    word32 kdfAlgoId, pwriEncAlgoId, keyEncAlgoId, cekSz;
    byte* pkiMsg = in;
    word32 pkiMsgSz = inSz;
#ifndef NO_PKCS7_STREAM
    word32 tmpIdx = *idx;
#endif

    switch (pkcs7->state) {
        case WC_PKCS7_DECRYPT_PWRI:
        #ifndef NO_PKCS7_STREAM
            /*@TODO for now just get full buffer, needs divided up */
            if ((ret = wc_PKCS7_AddDataToStream(pkcs7, in, inSz,
                   (pkcs7->stream->maxLen - pkcs7->stream->totalRd) +
                   pkcs7->stream->length, &pkiMsg, idx)) != 0) {
                return ret;
            }

            pkiMsgSz = wc_PKCS7_GetMaxStream(pkcs7, PKCS7_DEFAULT_PEEK, in, inSz);
        #endif
            /* remove KeyDerivationAlgorithmIdentifier */
            if (pkiMsg[(*idx)++] != (ASN_CONSTRUCTED | ASN_CONTEXT_SPECIFIC | 0))
                return ASN_PARSE_E;

            if (GetLength(pkiMsg, idx, &length, pkiMsgSz) < 0)
                return ASN_PARSE_E;

            /* get KeyDerivationAlgorithmIdentifier */
            if (wc_GetContentType(pkiMsg, idx, &kdfAlgoId, pkiMsgSz) < 0)
                return ASN_PARSE_E;

            /* get KDF params SEQ */
            if (GetSequence(pkiMsg, idx, &length, pkiMsgSz) < 0)
                return ASN_PARSE_E;

            /* get KDF salt OCTET STRING */
            if ( (pkiMsgSz > ((*idx) + 1)) &&
                 (pkiMsg[(*idx)++] != ASN_OCTET_STRING) ) {
                return ASN_PARSE_E;
            }

            if (GetLength(pkiMsg, idx, &saltSz, pkiMsgSz) < 0)
                return ASN_PARSE_E;

            salt = (byte*)XMALLOC(saltSz, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
            if (salt == NULL)
                return MEMORY_E;

            XMEMCPY(salt, pkiMsg + (*idx), saltSz);
            *idx += saltSz;

            /* get KDF iterations */
            if (GetMyVersion(pkiMsg, idx, &iterations, pkiMsgSz) < 0) {
                XFREE(salt, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
                return ASN_PARSE_E;
            }

            /* get KeyEncAlgoId SEQ */
            if (GetSequence(pkiMsg, idx, &length, pkiMsgSz) < 0) {
                XFREE(salt, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
                return ASN_PARSE_E;
            }

            /* get KeyEncAlgoId */
            if (wc_GetContentType(pkiMsg, idx, &keyEncAlgoId, pkiMsgSz) < 0) {
                XFREE(salt, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
                return ASN_PARSE_E;
            }

            /* get pwriEncAlgoId */
            if (GetAlgoId(pkiMsg, idx, &pwriEncAlgoId, oidBlkType, pkiMsgSz) < 0) {
                XFREE(salt, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
                return ASN_PARSE_E;
            }

            blockSz = wc_PKCS7_GetOIDBlockSize(pwriEncAlgoId);
            if (blockSz < 0) {
                XFREE(salt, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
                return blockSz;
            }

            /* get content-encryption key size, based on algorithm */
            kekKeySz = wc_PKCS7_GetOIDKeySize(pwriEncAlgoId);
            if (kekKeySz < 0) {
                XFREE(salt, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
                return kekKeySz;
            }

            /* get block cipher IV, stored in OPTIONAL parameter of AlgoID */
            if ( (pkiMsgSz > ((*idx) + 1)) &&
                 (pkiMsg[(*idx)++] != ASN_OCTET_STRING) ) {
                XFREE(salt, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
                return ASN_PARSE_E;
            }

            if (GetLength(pkiMsg, idx, &length, pkiMsgSz) < 0) {
                XFREE(salt, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
                return ASN_PARSE_E;
            }

            if (length != blockSz) {
                WOLFSSL_MSG("Incorrect IV length, must be of content alg block size");
                XFREE(salt, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
                return ASN_PARSE_E;
            }

            XMEMCPY(tmpIv, pkiMsg + (*idx), length);
            *idx += length;

            /* get EncryptedKey */
            if ( (pkiMsgSz < ((*idx) + 1)) ||
                 (pkiMsg[(*idx)++] != ASN_OCTET_STRING) ) {
                XFREE(salt, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
                return ASN_PARSE_E;
            }

            if (GetLength(pkiMsg, idx, &length, pkiMsgSz) < 0) {
                XFREE(salt, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
                return ASN_PARSE_E;
            }

            /* allocate temporary space for decrypted key */
            cekSz = length;
            cek = (byte*)XMALLOC(cekSz, pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
            if (cek == NULL) {
                XFREE(salt, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
                return MEMORY_E;
            }

            /* generate KEK */
            kek = (byte*)XMALLOC(kekKeySz, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
            if (kek == NULL) {
                XFREE(salt, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
                XFREE(cek, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
                return MEMORY_E;
            }

            ret = wc_PKCS7_GenerateKEK_PWRI(pkcs7, pkcs7->pass, pkcs7->passSz,
                                            salt, saltSz, kdfAlgoId, hashOID,
                                            iterations, kek, kekKeySz);
            if (ret < 0) {
                XFREE(salt, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
                XFREE(kek, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
                XFREE(cek, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
                return ASN_PARSE_E;
            }

            /* decrypt CEK with KEK */
            ret = wc_PKCS7_PwriKek_KeyUnWrap(pkcs7, kek, kekKeySz,
                                             pkiMsg + (*idx), length, cek,
                                             cekSz, tmpIv, blockSz,
                                             pwriEncAlgoId);
            if (ret < 0) {
                XFREE(salt, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
                XFREE(kek, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
                XFREE(cek, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
                return ret;
            }
            cekSz = ret;

            if (*decryptedKeySz < cekSz) {
                WOLFSSL_MSG("Decrypted key buffer too small for CEK");
                XFREE(salt, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
                XFREE(kek, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
                XFREE(cek, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
                return BUFFER_E;
            }

            XMEMCPY(decryptedKey, cek, cekSz);
            *decryptedKeySz = cekSz;

            XFREE(salt, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
            XFREE(kek, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
            XFREE(cek, pkcs7->heap, DYNAMIC_TYPE_PKCS7);

            /* mark recipFound, since we only support one RecipientInfo for now */
            *recipFound = 1;
            *idx += length;
        #ifndef NO_PKCS7_STREAM
            if ((ret = wc_PKCS7_StreamEndCase(pkcs7, &tmpIdx, idx)) != 0) {
                break;
            }
        #endif
            ret = 0; /* success */
            break;

        default:
            WOLFSSL_MSG("PKCS7 PWRI unknown state");
            ret = BAD_FUNC_ARG;
    }

    return ret;
}

#endif /* NO_PWDBASED */

/* decode ASN.1 KEKRecipientInfo (kekri), return 0 on success,
 * < 0 on error */
static int wc_PKCS7_DecryptKekri(PKCS7* pkcs7, byte* in, word32 inSz,
                               word32* idx, byte* decryptedKey,
                               word32* decryptedKeySz, int* recipFound)
{
    int length, keySz, dateLen;
    byte* keyId = NULL;
    const byte* datePtr = NULL;
    byte  dateFormat;
    word32 keyIdSz, kekIdSz, keyWrapOID;

    int ret = 0;
    byte* pkiMsg    = in;
    word32 pkiMsgSz = inSz;
#ifndef NO_PKCS7_STREAM
    word32 tmpIdx = *idx;
#endif

    switch (pkcs7->state) {
        case WC_PKCS7_DECRYPT_KEKRI:
            //@TODO for now just get full buffer, needs divided up

        #ifndef NO_PKCS7_STREAM
            if ((ret = wc_PKCS7_AddDataToStream(pkcs7, in, inSz,
                   (pkcs7->stream->maxLen - pkcs7->stream->totalRd) +
                   pkcs7->stream->length, &pkiMsg, idx)) != 0) {
                return ret;
            }

            pkiMsgSz = wc_PKCS7_GetMaxStream(pkcs7, PKCS7_DEFAULT_PEEK, in, inSz);
        #endif
            /* remove KEKIdentifier */
            if (GetSequence(pkiMsg, idx, &length, pkiMsgSz) < 0)
                return ASN_PARSE_E;

            kekIdSz = length;

            if (pkiMsg[(*idx)++] != ASN_OCTET_STRING)
                return ASN_PARSE_E;

            if (GetLength(pkiMsg, idx, &length, pkiMsgSz) < 0)
                return ASN_PARSE_E;

            /* save keyIdentifier and length */
            keyId = pkiMsg;
            keyIdSz = length;
            *idx += keyIdSz;

            /* may have OPTIONAL GeneralizedTime */
            if ((*idx < kekIdSz) && (pkiMsg[*idx] == ASN_GENERALIZED_TIME)) {
                if (wc_GetDateInfo(pkiMsg + *idx, pkiMsgSz, &datePtr, &dateFormat,
                                   &dateLen) != 0) {
                    return ASN_PARSE_E;
                }
                *idx += (dateLen + 1);
            }

            /* may have OPTIONAL OtherKeyAttribute */
            if ((*idx < kekIdSz) && (pkiMsg[*idx] ==
                                     (ASN_SEQUENCE | ASN_CONSTRUCTED))) {

                if (GetSequence(pkiMsg, idx, &length, pkiMsgSz) < 0)
                    return ASN_PARSE_E;

                /* skip it */
                *idx += length;
            }

            /* get KeyEncryptionAlgorithmIdentifier */
            if (GetAlgoId(pkiMsg, idx, &keyWrapOID, oidKeyWrapType, pkiMsgSz) < 0)
                return ASN_PARSE_E;

            /* get EncryptedKey */
            if (pkiMsg[(*idx)++] != ASN_OCTET_STRING)
                return ASN_PARSE_E;

            if (GetLength(pkiMsg, idx, &length, pkiMsgSz) < 0)
                return ASN_PARSE_E;

            /* decrypt CEK with KEK */
            keySz = wc_PKCS7_KeyWrap(pkiMsg + *idx, length, pkcs7->privateKey,
                                     pkcs7->privateKeySz, decryptedKey, *decryptedKeySz,
                                     keyWrapOID, AES_DECRYPTION);
            if (keySz <= 0)
                return keySz;

            *decryptedKeySz = (word32)keySz;

            /* mark recipFound, since we only support one RecipientInfo for now */
            *recipFound = 1;
            *idx += length;

        #ifndef NO_PKCS7_STREAM
            if ((ret = wc_PKCS7_StreamEndCase(pkcs7, &tmpIdx, idx)) != 0) {
                break;
            }
        #endif
            ret = 0; /* success */
            break;

        default:
            WOLFSSL_MSG("PKCS7 KEKRI unknown state");
            ret = BAD_FUNC_ARG;

    }

    (void)keyId;
    return ret;
}


/* decode ASN.1 KeyAgreeRecipientInfo (kari), return 0 on success,
 * < 0 on error */
static int wc_PKCS7_DecryptKari(PKCS7* pkcs7, byte* in, word32 inSz,
                               word32* idx, byte* decryptedKey,
                               word32* decryptedKeySz, int* recipFound)
{
#ifdef HAVE_ECC
    int ret, keySz;
    int encryptedKeySz;
    int direction = 0;
    word32 keyAgreeOID, keyWrapOID;

#ifdef WOLFSSL_SMALL_STACK
    byte* encryptedKey;
#else
    byte  encryptedKey[MAX_ENCRYPTED_KEY_SZ];
#endif

    byte* pkiMsg    = in;
    word32 pkiMsgSz = inSz;
#ifndef NO_PKCS7_STREAM
    word32 tmpIdx = (idx)? *idx : 0;
#endif

    if (pkcs7 == NULL || pkcs7->singleCert == NULL ||
        pkcs7->singleCertSz == 0 || pkiMsg == NULL ||
        idx == NULL || decryptedKey == NULL || decryptedKeySz == NULL) {
        return BAD_FUNC_ARG;
    }

    switch (pkcs7->state) {
        case WC_PKCS7_DECRYPT_KARI: {
            //@TODO for now just get full buffer, needs divided up

        #ifndef NO_PKCS7_STREAM
            if ((ret = wc_PKCS7_AddDataToStream(pkcs7, in, inSz,
                   (pkcs7->stream->maxLen - pkcs7->stream->totalRd) +
                   pkcs7->stream->length, &pkiMsg, idx)) != 0) {
                return ret;
            }

            pkiMsgSz = wc_PKCS7_GetMaxStream(pkcs7, PKCS7_DEFAULT_PEEK, in, inSz);
        #endif
            WC_PKCS7_KARI* kari;

            kari = wc_PKCS7_KariNew(pkcs7, WC_PKCS7_DECODE);
            if (kari == NULL)
                return MEMORY_E;

        #ifdef WOLFSSL_SMALL_STACK
            encryptedKey = (byte*)XMALLOC(MAX_ENCRYPTED_KEY_SZ, pkcs7->heap,
                                          DYNAMIC_TYPE_PKCS7);
            if (encryptedKey == NULL) {
                wc_PKCS7_KariFree(kari);
                return MEMORY_E;
            }
        #endif
            encryptedKeySz = MAX_ENCRYPTED_KEY_SZ;

            /* parse cert and key */
            ret = wc_PKCS7_KariParseRecipCert(kari, (byte*)pkcs7->singleCert,
                                              pkcs7->singleCertSz, pkcs7->privateKey,
                                              pkcs7->privateKeySz);
            if (ret != 0) {
                wc_PKCS7_KariFree(kari);
                #ifdef WOLFSSL_SMALL_STACK
                    XFREE(encryptedKey, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
                #endif
                return ret;
            }

            /* remove OriginatorIdentifierOrKey */
            ret = wc_PKCS7_KariGetOriginatorIdentifierOrKey(kari, pkiMsg,
                                                            pkiMsgSz, idx);
            if (ret != 0) {
                wc_PKCS7_KariFree(kari);
                #ifdef WOLFSSL_SMALL_STACK
                    XFREE(encryptedKey, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
                #endif
                return ret;
            }

            /* try and remove optional UserKeyingMaterial */
            ret = wc_PKCS7_KariGetUserKeyingMaterial(kari, pkiMsg, pkiMsgSz, idx);
            if (ret != 0) {
                wc_PKCS7_KariFree(kari);
                #ifdef WOLFSSL_SMALL_STACK
                    XFREE(encryptedKey, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
                #endif
                return ret;
            }

            /* remove KeyEncryptionAlgorithmIdentifier */
            ret = wc_PKCS7_KariGetKeyEncryptionAlgorithmId(kari, pkiMsg, pkiMsgSz,
                                                           idx, &keyAgreeOID,
                                                           &keyWrapOID);
            if (ret != 0) {
                wc_PKCS7_KariFree(kari);
                #ifdef WOLFSSL_SMALL_STACK
                    XFREE(encryptedKey, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
                #endif
                return ret;
            }

            /* if user has not explicitly set keyAgreeOID, set from one in bundle */
            if (pkcs7->keyAgreeOID == 0)
                pkcs7->keyAgreeOID = keyAgreeOID;

            /* set direction based on key wrap algorithm */
            switch (keyWrapOID) {
        #ifndef NO_AES
            #ifdef WOLFSSL_AES_128
                case AES128_WRAP:
            #endif
            #ifdef WOLFSSL_AES_192
                case AES192_WRAP:
            #endif
            #ifdef WOLFSSL_AES_256
                case AES256_WRAP:
            #endif
                    direction = AES_DECRYPTION;
                    break;
        #endif
                default:
                    wc_PKCS7_KariFree(kari);
                    #ifdef WOLFSSL_SMALL_STACK
                        XFREE(encryptedKey, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
                    #endif
                    WOLFSSL_MSG("AES key wrap algorithm unsupported");
                    return BAD_KEYWRAP_ALG_E;
            }

            /* remove RecipientEncryptedKeys */
            ret = wc_PKCS7_KariGetRecipientEncryptedKeys(kari, pkiMsg, pkiMsgSz,
                                       idx, recipFound, encryptedKey, &encryptedKeySz);
            if (ret != 0) {
                wc_PKCS7_KariFree(kari);
                #ifdef WOLFSSL_SMALL_STACK
                    XFREE(encryptedKey, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
                #endif
                return ret;
            }

            /* create KEK */
            ret = wc_PKCS7_KariGenerateKEK(kari, keyWrapOID, pkcs7->keyAgreeOID);
            if (ret != 0) {
                wc_PKCS7_KariFree(kari);
                #ifdef WOLFSSL_SMALL_STACK
                    XFREE(encryptedKey, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
                #endif
                return ret;
            }

            /* decrypt CEK with KEK */
            keySz = wc_PKCS7_KeyWrap(encryptedKey, encryptedKeySz, kari->kek,
                                     kari->kekSz, decryptedKey, *decryptedKeySz,
                                     keyWrapOID, direction);
            if (keySz <= 0) {
                wc_PKCS7_KariFree(kari);
                #ifdef WOLFSSL_SMALL_STACK
                    XFREE(encryptedKey, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
                #endif
                return keySz;
            }
            *decryptedKeySz = (word32)keySz;

            wc_PKCS7_KariFree(kari);
            #ifdef WOLFSSL_SMALL_STACK
                XFREE(encryptedKey, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
            #endif
            #ifndef NO_PKCS7_STREAM
            if ((ret = wc_PKCS7_StreamEndCase(pkcs7, &tmpIdx, idx)) != 0) {
                break;
            }
            #endif
            ret = 0; /* success */
        }
        break;

        default:
            WOLFSSL_MSG("PKCS7 kari unknown state");
            ret = BAD_FUNC_ARG;

    }
    return ret;

    (void)pkiMsg;
    (void)pkiMsgSz;
#else
    (void)in;
    (void)inSz;
    (void)pkcs7;
    (void)idx;
    (void)decryptedKey;
    (void)decryptedKeySz;
    (void)recipFound;

    return NOT_COMPILED_IN;
#endif /* HAVE_ECC */
}


/* decode ASN.1 RecipientInfos SET, return 0 on success, < 0 on error */
static int wc_PKCS7_DecryptRecipientInfos(PKCS7* pkcs7, byte* in,
                            word32  inSz, word32* idx, byte* decryptedKey,
                            word32* decryptedKeySz, int* recipFound)
{
    word32 savedIdx;
    int version, ret = 0, length;
    byte* pkiMsg = in;
    word32 pkiMsgSz = inSz;
#ifndef NO_PKCS7_STREAM
    word32 tmpIdx = *idx;
#endif

    if (pkcs7 == NULL || pkiMsg == NULL || idx == NULL ||
        decryptedKey == NULL || decryptedKeySz == NULL ||
        recipFound == NULL) {
        return BAD_FUNC_ARG;
    }

    /* check if in the process of decrypting */
    switch (pkcs7->state) {
        case WC_PKCS7_DECRYPT_KTRI:
        case WC_PKCS7_DECRYPT_KTRI_2:
        case WC_PKCS7_DECRYPT_KTRI_3:
        #ifndef NO_RSA
            ret = wc_PKCS7_DecryptKtri(pkcs7, in, inSz, idx,
                                      decryptedKey, decryptedKeySz, recipFound);
        #else
            return NOT_COMPILED_IN;
        #endif
            break;

        case WC_PKCS7_DECRYPT_KARI:
                ret = wc_PKCS7_DecryptKari(pkcs7, in, inSz, idx,
                                      decryptedKey, decryptedKeySz, recipFound);
                break;

        case WC_PKCS7_DECRYPT_KEKRI:
                ret = wc_PKCS7_DecryptKekri(pkcs7, in, inSz, idx,
                                      decryptedKey, decryptedKeySz, recipFound);
                break;

        case WC_PKCS7_DECRYPT_PWRI:
        #ifndef NO_PWDBASED
                ret = wc_PKCS7_DecryptPwri(pkcs7, in, inSz, idx,
                                      decryptedKey, decryptedKeySz, recipFound);
        #else
                return NOT_COMPILED_IN;
        #endif
                break;

        case WC_PKCS7_DECRYPT_ORI:
            ret = wc_PKCS7_DecryptOri(pkcs7, in, inSz, idx,
                                      decryptedKey, decryptedKeySz, recipFound);
            break;

        default:
            /* not in decrypting state */
            break;
    }

    if (ret < 0) {
        return ret;
    }

    savedIdx = *idx;
#ifndef NO_PKCS7_STREAM
    pkiMsgSz = wc_PKCS7_GetMaxStream(pkcs7, PKCS7_DEFAULT_PEEK, in, inSz);
    if (pkcs7->stream->length > 0) pkiMsg = pkcs7->stream->buffer;
#endif

    /* when looking for next recipient, use first sequence and version to
     * indicate there is another, if not, move on */
    while(*recipFound == 0) {

        /* remove RecipientInfo, if we don't have a SEQUENCE, back up idx to
         * last good saved one */
        if (GetSequence(pkiMsg, idx, &length, pkiMsgSz) > 0) {

        #ifndef NO_RSA
            /* found ktri */
            #ifndef NO_PKCS7_STREAM
            if ((ret = wc_PKCS7_StreamEndCase(pkcs7, &tmpIdx, idx)) != 0) {
                break;
            }
            #endif
            wc_PKCS7_ChangeState(pkcs7, WC_PKCS7_DECRYPT_KTRI);
            ret = wc_PKCS7_DecryptKtri(pkcs7, in, inSz, idx,
                                      decryptedKey, decryptedKeySz,
                                      recipFound);
            if (ret != 0)
                return ret;
        #else
            return NOT_COMPILED_IN;
        #endif
        }
        else {
            /* kari is IMPLICIT[1] */
            *idx = savedIdx;
            if (pkiMsg[*idx] == (ASN_CONSTRUCTED | ASN_CONTEXT_SPECIFIC | 1)) {

                (*idx)++;

                if (GetLength(pkiMsg, idx, &length, pkiMsgSz) < 0)
                    return ASN_PARSE_E;

                if (GetMyVersion(pkiMsg, idx, &version, pkiMsgSz) < 0) {
                    *idx = savedIdx;
                    break;
                }

                if (version != 3)
                    return ASN_VERSION_E;

                /* found kari */
            #ifndef NO_PKCS7_STREAM
                if ((ret = wc_PKCS7_StreamEndCase(pkcs7, &tmpIdx, idx)) != 0) {
                    break;
                }
            #endif
                wc_PKCS7_ChangeState(pkcs7, WC_PKCS7_DECRYPT_KARI);
                ret = wc_PKCS7_DecryptKari(pkcs7, in, inSz, idx,
                                          decryptedKey, decryptedKeySz,
                                          recipFound);
                if (ret != 0)
                    return ret;

            /* kekri is IMPLICIT[2] */
            } else if (pkiMsg[*idx] == (ASN_CONSTRUCTED |
                                        ASN_CONTEXT_SPECIFIC | 2)) {
                (*idx)++;

                if (GetLength(pkiMsg, idx, &version, pkiMsgSz) < 0)
                    return ASN_PARSE_E;

                if (GetMyVersion(pkiMsg, idx, &version, pkiMsgSz) < 0) {
                    *idx = savedIdx;
                    break;
                }

                if (version != 4)
                    return ASN_VERSION_E;

                /* found kekri */
            #ifndef NO_PKCS7_STREAM
                if ((ret = wc_PKCS7_StreamEndCase(pkcs7, &tmpIdx, idx)) != 0) {
                    break;
                }
            #endif
                wc_PKCS7_ChangeState(pkcs7, WC_PKCS7_DECRYPT_KEKRI);
                ret = wc_PKCS7_DecryptKekri(pkcs7, in, inSz, idx,
                                           decryptedKey, decryptedKeySz,
                                           recipFound);
                if (ret != 0)
                    return ret;

            /* pwri is IMPLICIT[3] */
            } else if (pkiMsg[*idx] == (ASN_CONSTRUCTED |
                                        ASN_CONTEXT_SPECIFIC | 3)) {
        #ifndef NO_PWDBASED
                (*idx)++;

                if (GetLength(pkiMsg, idx, &version, pkiMsgSz) < 0)
                    return ASN_PARSE_E;

                if (GetMyVersion(pkiMsg, idx, &version, pkiMsgSz) < 0) {
                    *idx = savedIdx;
                    break;
                }

                if (version != 0)
                    return ASN_VERSION_E;

                /* found pwri */
            #ifndef NO_PKCS7_STREAM
                if ((ret = wc_PKCS7_StreamEndCase(pkcs7, &tmpIdx, idx)) != 0) {
                    break;
                }
            #endif
                wc_PKCS7_ChangeState(pkcs7, WC_PKCS7_DECRYPT_PWRI);
                ret = wc_PKCS7_DecryptPwri(pkcs7, in, inSz, idx,
                                           decryptedKey, decryptedKeySz,
                                           recipFound);
                if (ret != 0)
                    return ret;
        #else
                return NOT_COMPILED_IN;
        #endif

            /* ori is IMPLICIT[4] */
            } else if (pkiMsg[*idx] == (ASN_CONSTRUCTED |
                                        ASN_CONTEXT_SPECIFIC | 4)) {
                (*idx)++;

                /* found ori */
            #ifndef NO_PKCS7_STREAM
                if ((ret = wc_PKCS7_StreamEndCase(pkcs7, &tmpIdx, idx)) != 0) {
                    break;
                }
            #endif
                wc_PKCS7_ChangeState(pkcs7, WC_PKCS7_DECRYPT_ORI);
                ret = wc_PKCS7_DecryptOri(pkcs7, in, inSz, idx,
                                          decryptedKey, decryptedKeySz,
                                          recipFound);
                if (ret != 0)
                    return ret;

            } else {
                /* failed to find RecipientInfo, restore idx and continue */
                *idx = savedIdx;
                break;
            }
        }

        /* update good idx */
        savedIdx = *idx;
    }

    return ret;
}


/* Parse encoded EnvelopedData bundle up to RecipientInfo set.
 *
 * return size of RecipientInfo SET on success, negative upon error */
static int wc_PKCS7_ParseToRecipientInfoSet(PKCS7* pkcs7, byte* in,
                                            word32 inSz, word32* idx,
                                            int type)
{
    int version = 0, length, ret = 0;
    word32 contentType;
    byte* pkiMsg = in;
    word32 pkiMsgSz = inSz;
#ifndef NO_PKCS7_STREAM
    word32 tmpIdx = 0;
#endif

    if (pkcs7 == NULL || pkiMsg == NULL || pkiMsgSz == 0 || idx == NULL)
        return BAD_FUNC_ARG;

    if ((type != ENVELOPED_DATA) && (type != AUTH_ENVELOPED_DATA))
        return BAD_FUNC_ARG;

#ifndef NO_PKCS7_STREAM
    if (pkcs7->stream == NULL) {
        if ((ret = wc_PKCS7_CreateStream(pkcs7)) != 0) {
            return ret;
        }
    }
#endif

    switch (pkcs7->state) {
        case WC_PKCS7_INFOSET_START:
        case WC_PKCS7_INFOSET_BER:
        case WC_PKCS7_INFOSET_STAGE1:
        case WC_PKCS7_INFOSET_STAGE2:
        case WC_PKCS7_INFOSET_END:
            break;

        default:
            WOLFSSL_MSG("Warning, setting PKCS7 info state to start");
            wc_PKCS7_ChangeState(pkcs7, WC_PKCS7_INFOSET_START);
    }

    switch (pkcs7->state) {
        case WC_PKCS7_INFOSET_START:
        #ifndef NO_PKCS7_STREAM
            if ((ret = wc_PKCS7_AddDataToStream(pkcs7, in, inSz, MAX_SEQ_SZ +
                            ASN_TAG_SZ, &pkiMsg, idx)) != 0) {
                return ret;
            }

            pkiMsgSz = wc_PKCS7_GetMaxStream(pkcs7, PKCS7_SEQ_PEEK, in, inSz);
        #endif
            /* read past ContentInfo, verify type is envelopedData */
            if (ret == 0 && GetSequence(pkiMsg, idx, &length, pkiMsgSz) < 0)
            {
                ret = ASN_PARSE_E;
            }

            if (ret == 0 && length == 0 && pkiMsg[(*idx)-1] == 0x80) {
        #ifdef ASN_BER_TO_DER
                word32 len;

                wc_PKCS7_ChangeState(pkcs7, WC_PKCS7_INFOSET_BER);
                FALL_THROUGH;

                /* full buffer is needed for conversion */
                case WC_PKCS7_INFOSET_BER:
                #ifndef NO_PKCS7_STREAM
                if ((ret = wc_PKCS7_AddDataToStream(pkcs7, in, inSz,
                            pkcs7->stream->maxLen - pkcs7->stream->length,
                            &pkiMsg, idx)) != 0) {
                    return ret;
                }

                pkiMsgSz = wc_PKCS7_GetMaxStream(pkcs7, PKCS7_DEFAULT_PEEK, in, inSz);
                #endif

                len = 0;

                ret = wc_BerToDer(pkiMsg, pkiMsgSz, NULL, &len);
                if (ret != LENGTH_ONLY_E)
                    return ret;
                pkcs7->der = (byte*)XMALLOC(len, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
                if (pkcs7->der == NULL)
                    return MEMORY_E;
                ret = wc_BerToDer(pkiMsg, pkiMsgSz, pkcs7->der, &len);
                if (ret < 0)
                    return ret;

                pkiMsg = pkcs7->der;
                pkiMsgSz = len;
                *idx = 0;
                if (GetSequence(pkiMsg, idx, &length, pkiMsgSz) < 0)
                    return ASN_PARSE_E;
        #else
                return BER_INDEF_E;
        #endif
            }
        #ifndef NO_PKCS7_STREAM
            if ((ret = wc_PKCS7_StreamEndCase(pkcs7, &tmpIdx, idx)) != 0) {
                break;
            }
        #endif
            wc_PKCS7_ChangeState(pkcs7, WC_PKCS7_INFOSET_STAGE1);
            FALL_THROUGH;

        case WC_PKCS7_INFOSET_STAGE1:
        #ifndef NO_PKCS7_STREAM
            if ((ret = wc_PKCS7_AddDataToStream(pkcs7, in, inSz, MAX_OID_SZ +
                            MAX_LENGTH_SZ + ASN_TAG_SZ, &pkiMsg, idx)) != 0) {
                return ret;
            }

            pkiMsgSz = wc_PKCS7_GetMaxStream(pkcs7, PKCS7_DEFAULT_PEEK, in, inSz);
        #endif
            if (ret == 0 && wc_GetContentType(pkiMsg, idx, &contentType, pkiMsgSz) < 0)
                ret = ASN_PARSE_E;

            if (ret == 0) {
                if (type == ENVELOPED_DATA && contentType != ENVELOPED_DATA) {
                    WOLFSSL_MSG("PKCS#7 input not of type EnvelopedData");
                    ret = PKCS7_OID_E;
                } else if (type == AUTH_ENVELOPED_DATA &&
                       contentType != AUTH_ENVELOPED_DATA) {
                    WOLFSSL_MSG("PKCS#7 input not of type AuthEnvelopedData");
                    ret = PKCS7_OID_E;
                }
            }

            if (ret == 0 && pkiMsg[(*idx)++] !=
                    (ASN_CONSTRUCTED | ASN_CONTEXT_SPECIFIC | 0))
                ret = ASN_PARSE_E;

            if (ret == 0 && GetLength(pkiMsg, idx, &length, pkiMsgSz) < 0)
                ret = ASN_PARSE_E;

            if (ret < 0)
                break;

        #ifndef NO_PKCS7_STREAM
            if ((ret = wc_PKCS7_StreamEndCase(pkcs7, &tmpIdx, idx)) != 0) {
                    break;
            }
        #endif
            wc_PKCS7_ChangeState(pkcs7, WC_PKCS7_INFOSET_STAGE2);
            FALL_THROUGH;

        case WC_PKCS7_INFOSET_STAGE2:
        #ifndef NO_PKCS7_STREAM
            if ((ret = wc_PKCS7_AddDataToStream(pkcs7, in, inSz, MAX_SEQ_SZ +
                            MAX_VERSION_SZ, &pkiMsg, idx)) != 0) {
                return ret;
            }

            pkiMsgSz = wc_PKCS7_GetMaxStream(pkcs7, PKCS7_DEFAULT_PEEK, in, inSz);
        #endif
            /* remove EnvelopedData and version */
            if (ret == 0 && GetSequence(pkiMsg, idx, &length, pkiMsgSz) < 0)
                ret = ASN_PARSE_E;

            if (ret == 0 && GetMyVersion(pkiMsg, idx, &version, pkiMsgSz) < 0)
                ret = ASN_PARSE_E;

            if (ret < 0)
                break;

        #ifndef NO_PKCS7_STREAM
            if ((ret = wc_PKCS7_StreamEndCase(pkcs7, &tmpIdx, idx)) != 0) {
                break;
            }

            pkcs7->stream->varOne = version;
        #endif
            wc_PKCS7_ChangeState(pkcs7, WC_PKCS7_INFOSET_END);
            FALL_THROUGH;

        case WC_PKCS7_INFOSET_END:
        #ifndef NO_PKCS7_STREAM
            if ((ret = wc_PKCS7_AddDataToStream(pkcs7, in, inSz,
                            MAX_SET_SZ, &pkiMsg, idx)) != 0) {
                return ret;
            }

            pkiMsgSz = wc_PKCS7_GetMaxStream(pkcs7, PKCS7_DEFAULT_PEEK, in, inSz);
            version = pkcs7->stream->varOne;
        #endif

            if (type == ENVELOPED_DATA) {
                /* TODO :: make this more accurate */
                if ((pkcs7->publicKeyOID == RSAk &&
                     (version != 0 && version != 2))
                #ifdef HAVE_ECC
                        || (pkcs7->publicKeyOID == ECDSAk &&
                            (version != 0 && version != 2 && version != 3))
                #endif
                        ) {
                    WOLFSSL_MSG("PKCS#7 envelopedData version incorrect");
                    ret = ASN_VERSION_E;
                }
            } else {
                /* AuthEnvelopedData version MUST be 0 */
                if (version != 0) {
                    WOLFSSL_MSG("PKCS#7 AuthEnvelopedData needs to be of version 0");
                    ret = ASN_VERSION_E;
                }
            }

            /* remove RecipientInfo set, get length of set */
            if (ret == 0 && GetSet(pkiMsg, idx, &length, pkiMsgSz) < 0)
                ret = ASN_PARSE_E;

            if (ret < 0)
                break;

        #ifndef NO_PKCS7_STREAM
            if ((ret = wc_PKCS7_StreamEndCase(pkcs7, &tmpIdx, idx)) != 0) {
                break;
            }
        #endif

            if (ret == 0)
                ret = length;

            break;

        default:
            WOLFSSL_MSG("Bad PKCS7 info set state");
            ret = BAD_FUNC_ARG;
            break;
    }

    return ret;
}


/* Import secret/private key into a PKCS7 structure. Used for setting
 * the secret key for decryption a EnvelopedData KEKRI RecipientInfo.
 *
 * Returns 0 on success, negative upon error */
WOLFSSL_API int wc_PKCS7_SetKey(PKCS7* pkcs7, byte* key, word32 keySz)
{
    if (pkcs7 == NULL || key == NULL || keySz == 0)
        return BAD_FUNC_ARG;

    pkcs7->privateKey = key;
    pkcs7->privateKeySz = keySz;

    return 0;
}


/* unwrap and decrypt PKCS#7 envelopedData object, return decoded size */
WOLFSSL_API int wc_PKCS7_DecodeEnvelopedData(PKCS7* pkcs7, byte* in,
                                         word32 inSz, byte* output,
                                         word32 outputSz)
{
    int recipFound = 0;
    int ret, length = 0;
    word32 idx = 0;
#ifndef NO_PKCS7_STREAM
    word32 tmpIdx = 0;
#endif
    word32 contentType, encOID = 0;
    word32 decryptedKeySz = MAX_ENCRYPTED_KEY_SZ;

    int expBlockSz = 0, blockKeySz = 0;
    byte  tmpIvBuf[MAX_CONTENT_IV_SIZE];
    byte* tmpIv = tmpIvBuf;

    byte* pkiMsg    = in;
    word32 pkiMsgSz = inSz;
    byte* decryptedKey = NULL;
    int encryptedContentSz;
    byte padLen;
    byte* encryptedContent = NULL;
    int explicitOctet;

    if (pkcs7 == NULL)
        return BAD_FUNC_ARG;

    if (pkiMsg == NULL || pkiMsgSz == 0 ||
        output == NULL || outputSz == 0)
        return BAD_FUNC_ARG;

#ifndef NO_PKCS7_STREAM
    (void)tmpIv; /* help out static analysis */
    if (pkcs7->stream == NULL) {
        if ((ret = wc_PKCS7_CreateStream(pkcs7)) != 0) {
            return ret;
        }
    }
#endif

    switch (pkcs7->state) {
        case WC_PKCS7_START:
        case WC_PKCS7_INFOSET_START:
        case WC_PKCS7_INFOSET_BER:
        case WC_PKCS7_INFOSET_STAGE1:
        case WC_PKCS7_INFOSET_STAGE2:
        case WC_PKCS7_INFOSET_END:
            ret = wc_PKCS7_ParseToRecipientInfoSet(pkcs7, pkiMsg, pkiMsgSz,
                    &idx, ENVELOPED_DATA);
            if (ret < 0) {
                break;
            }

            decryptedKey = (byte*)XMALLOC(MAX_ENCRYPTED_KEY_SZ, pkcs7->heap,
                                                       DYNAMIC_TYPE_PKCS7);
            if (decryptedKey == NULL)
                return MEMORY_E;
            wc_PKCS7_ChangeState(pkcs7, WC_PKCS7_ENV_2);
        #ifndef NO_PKCS7_STREAM
            tmpIdx = idx;
            pkcs7->stream->aad = decryptedKey;
        #endif
            FALL_THROUGH;

        case WC_PKCS7_ENV_2:
        #ifndef NO_PKCS7_STREAM
            /* store up enough buffer for initial info set decode */
            if ((ret = wc_PKCS7_AddDataToStream(pkcs7, in, inSz, MAX_LENGTH_SZ +
                            MAX_VERSION_SZ + ASN_TAG_SZ, &pkiMsg, &idx)) != 0) {
                return ret;
            }
        #endif
            FALL_THROUGH;

        case WC_PKCS7_DECRYPT_KTRI:
        case WC_PKCS7_DECRYPT_KTRI_2:
        case WC_PKCS7_DECRYPT_KTRI_3:
        case WC_PKCS7_DECRYPT_KARI:
        case WC_PKCS7_DECRYPT_KEKRI:
        case WC_PKCS7_DECRYPT_PWRI:
        case WC_PKCS7_DECRYPT_ORI:
        #ifndef NO_PKCS7_STREAM
            decryptedKey   = pkcs7->stream->aad;
            decryptedKeySz = MAX_ENCRYPTED_KEY_SZ;
        #endif

            ret = wc_PKCS7_DecryptRecipientInfos(pkcs7, in, inSz, &idx,
                                        decryptedKey, &decryptedKeySz,
                                        &recipFound);
            if (ret == 0 && recipFound == 0) {
                WOLFSSL_MSG("No recipient found in envelopedData that matches input");
                ret = PKCS7_RECIP_E;
            }

            if (ret != 0)
                break;
        #ifndef NO_PKCS7_STREAM
            tmpIdx               = idx;
            pkcs7->stream->aadSz = decryptedKeySz;
        #endif
            wc_PKCS7_ChangeState(pkcs7, WC_PKCS7_ENV_3);
            FALL_THROUGH;

        case WC_PKCS7_ENV_3:

        #ifndef NO_PKCS7_STREAM
            if ((ret = wc_PKCS7_AddDataToStream(pkcs7, in, inSz, MAX_LENGTH_SZ +
                                                MAX_VERSION_SZ + ASN_TAG_SZ +
                                                MAX_LENGTH_SZ, &pkiMsg, &idx))
                                                != 0) {
                return ret;
            }

            pkiMsgSz = wc_PKCS7_GetMaxStream(pkcs7, PKCS7_SEQ_PEEK, in, inSz);
        #endif

            /* remove EncryptedContentInfo */
            if (ret == 0 && GetSequence(pkiMsg, &idx, &length, pkiMsgSz) < 0) {
                ret = ASN_PARSE_E;
            }

            if (ret == 0 && wc_GetContentType(pkiMsg, &idx, &contentType,
                        pkiMsgSz) < 0) {
                ret = ASN_PARSE_E;
            }

            if (ret == 0 && GetAlgoId(pkiMsg, &idx, &encOID, oidBlkType,
                        pkiMsgSz) < 0) {
                ret = ASN_PARSE_E;
            }

            blockKeySz = wc_PKCS7_GetOIDKeySize(encOID);
            if (ret == 0 && blockKeySz < 0) {
                ret = blockKeySz;
            }

            expBlockSz = wc_PKCS7_GetOIDBlockSize(encOID);
            if (ret == 0 && expBlockSz < 0) {
                ret = expBlockSz;
            }

            /* get block cipher IV, stored in OPTIONAL parameter of AlgoID */
            if (ret == 0 && pkiMsg[idx++] != ASN_OCTET_STRING) {
                ret = ASN_PARSE_E;
            }

            if (ret == 0 && GetLength(pkiMsg, &idx, &length, pkiMsgSz) < 0) {
                ret = ASN_PARSE_E;
            }

            if (ret == 0 && length != expBlockSz) {
                WOLFSSL_MSG("Incorrect IV length, must be of content alg block size");
                ret = ASN_PARSE_E;
            }

            if (ret != 0)
                break;
        #ifndef NO_PKCS7_STREAM
            if ((ret = wc_PKCS7_StreamEndCase(pkcs7, &tmpIdx, &idx)) != 0) {
                break;
            }
            wc_PKCS7_StreamStoreVar(pkcs7, encOID, expBlockSz, length);
            pkcs7->stream->contentSz = blockKeySz;
            pkcs7->stream->expected = length + MAX_LENGTH_SZ + MAX_LENGTH_SZ +
                ASN_TAG_SZ + ASN_TAG_SZ;
        #endif
            wc_PKCS7_ChangeState(pkcs7, WC_PKCS7_ENV_4);
            FALL_THROUGH;

        case WC_PKCS7_ENV_4:

        #ifndef NO_PKCS7_STREAM
            if ((ret = wc_PKCS7_AddDataToStream(pkcs7, in, inSz,
                            pkcs7->stream->expected, &pkiMsg, &idx)) != 0) {
                return ret;
            }

            pkiMsgSz = wc_PKCS7_GetMaxStream(pkcs7, PKCS7_SEQ_PEEK, in, inSz);
            wc_PKCS7_StreamGetVar(pkcs7, 0, 0, &length);
            tmpIv = pkcs7->stream->tmpIv;
        #endif

            XMEMCPY(tmpIv, &pkiMsg[idx], length);
            idx += length;

            explicitOctet = pkiMsg[idx] ==
                                   (ASN_CONTEXT_SPECIFIC | ASN_CONSTRUCTED | 0);

            /* read encryptedContent, cont[0] */
            if (ret == 0 && pkiMsg[idx] != (ASN_CONTEXT_SPECIFIC | 0) &&
                pkiMsg[idx] != (ASN_CONTEXT_SPECIFIC | ASN_CONSTRUCTED | 0)) {
                ret = ASN_PARSE_E;
            }
            idx++;

            if (ret == 0 && GetLength(pkiMsg, &idx, &encryptedContentSz,
                                                               pkiMsgSz) <= 0) {
                ret = ASN_PARSE_E;
            }

            if (ret == 0 && explicitOctet) {
                if (ret == 0 && pkiMsg[idx++] != ASN_OCTET_STRING) {
                    ret = ASN_PARSE_E;
                    break;
                }

                if (ret == 0 && GetLength(pkiMsg, &idx, &encryptedContentSz,
                            pkiMsgSz) <= 0) {
                    ret = ASN_PARSE_E;
                    break;
                }
            }

            if (ret != 0)
                break;

        #ifndef NO_PKCS7_STREAM
            if ((ret = wc_PKCS7_StreamEndCase(pkcs7, &tmpIdx, &idx)) != 0) {
                break;
            }
            pkcs7->stream->expected = encryptedContentSz;
            wc_PKCS7_StreamGetVar(pkcs7, &encOID, &expBlockSz, 0);
            wc_PKCS7_StreamStoreVar(pkcs7, encOID, expBlockSz,
                    encryptedContentSz);
        #endif
            wc_PKCS7_ChangeState(pkcs7, WC_PKCS7_ENV_5);
            FALL_THROUGH;

        case WC_PKCS7_ENV_5:

        #ifndef NO_PKCS7_STREAM
            if ((ret = wc_PKCS7_AddDataToStream(pkcs7, in, inSz,
                            pkcs7->stream->expected, &pkiMsg, &idx)) != 0) {
                return ret;
            }

            wc_PKCS7_StreamGetVar(pkcs7, &encOID, &expBlockSz,
                    &encryptedContentSz);
            tmpIv = pkcs7->stream->tmpIv;

            /* restore decrypted key */
            decryptedKey   = pkcs7->stream->aad;
            decryptedKeySz = pkcs7->stream->aadSz;
            blockKeySz = pkcs7->stream->contentSz;
        #endif
            encryptedContent = (byte*)XMALLOC(encryptedContentSz, pkcs7->heap,
                                                       DYNAMIC_TYPE_PKCS7);
            if (ret == 0 && encryptedContent == NULL) {
                ret = MEMORY_E;
                break;
            }

            if (ret == 0) {
                XMEMCPY(encryptedContent, &pkiMsg[idx], encryptedContentSz);
            }

            /* decrypt encryptedContent */
            ret = wc_PKCS7_DecryptContent(encOID, decryptedKey, blockKeySz,
                                  tmpIv, expBlockSz, NULL, 0, NULL, 0,
                                  encryptedContent, encryptedContentSz,
                                  encryptedContent);
            if (ret != 0) {
                XFREE(encryptedContent, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
                break;
            }

            padLen = encryptedContent[encryptedContentSz-1];

            /* copy plaintext to output */
            XMEMCPY(output, encryptedContent, encryptedContentSz - padLen);

            /* free memory, zero out keys */
            ForceZero(decryptedKey, MAX_ENCRYPTED_KEY_SZ);
            ForceZero(encryptedContent, encryptedContentSz);
            XFREE(encryptedContent, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
            XFREE(decryptedKey, pkcs7->heap, DYNAMIC_TYPE_PKCS7);

            ret = encryptedContentSz - padLen;
        #ifndef NO_PKCS7_STREAM
            pkcs7->stream->aad = NULL;
            pkcs7->stream->aadSz = 0;
            wc_PKCS7_ResetStream(pkcs7);
        #endif
            wc_PKCS7_ChangeState(pkcs7, WC_PKCS7_START);
            break;

        default:
            WOLFSSL_MSG("PKCS#7 unknown decode enveloped state");
            ret = BAD_FUNC_ARG;
    }

#ifndef NO_PKCS7_STREAM
    if (ret < 0 && ret != WC_PKCS7_WANT_READ_E) {
        wc_PKCS7_ResetStream(pkcs7);
        wc_PKCS7_ChangeState(pkcs7, WC_PKCS7_START);
    }
#else
    if (decryptedKey != NULL && ret < 0) {
        ForceZero(decryptedKey, MAX_ENCRYPTED_KEY_SZ);
        XFREE(decryptedKey, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
    }
#endif
    return ret;
}


/* build PKCS#7 authEnvelopedData content type, return enveloped size */
int wc_PKCS7_EncodeAuthEnvelopedData(PKCS7* pkcs7, byte* output,
                                     word32 outputSz)
{
    int ret, idx = 0;
    int totalSz, encryptedOutSz;

    int contentInfoSeqSz, outerContentTypeSz, outerContentSz;
    byte contentInfoSeq[MAX_SEQ_SZ];
    byte outerContentType[MAX_ALGO_SZ];
    byte outerContent[MAX_SEQ_SZ];

    int envDataSeqSz, verSz;
    byte envDataSeq[MAX_SEQ_SZ];
    byte ver[MAX_VERSION_SZ];

    WC_RNG rng;
    int blockSz, blockKeySz;
    byte* encryptedContent;

    Pkcs7EncodedRecip* tmpRecip = NULL;
    int recipSz, recipSetSz;
    byte recipSet[MAX_SET_SZ];

    int encContentOctetSz, encContentSeqSz, contentTypeSz;
    int contentEncAlgoSz, nonceOctetStringSz, macOctetStringSz;
    byte encContentSeq[MAX_SEQ_SZ];
    byte contentType[MAX_ALGO_SZ];
    byte contentEncAlgo[MAX_ALGO_SZ];
    byte nonceOctetString[MAX_OCTET_STR_SZ];
    byte encContentOctet[MAX_OCTET_STR_SZ];
    byte macOctetString[MAX_OCTET_STR_SZ];

    byte authTag[AES_BLOCK_SIZE];
    byte nonce[GCM_NONCE_MID_SZ];   /* GCM nonce is larger than CCM */
    byte macInt[MAX_VERSION_SZ];
    word32 nonceSz, macIntSz;

    /* authAttribs */
    byte* flatAuthAttribs = NULL;
    byte authAttribSet[MAX_SET_SZ];
    EncodedAttrib authAttribs[MAX_AUTH_ATTRIBS_SZ];
    word32 authAttribsSz = 0, authAttribsCount = 0;
    word32 authAttribsSetSz = 0;

    byte* aadBuffer = NULL;
    word32 aadBufferSz = 0;
    byte authAttribAadSet[MAX_SET_SZ];
    word32 authAttribsAadSetSz = 0;

    /* unauthAttribs */
    byte* flatUnauthAttribs = NULL;
    byte unauthAttribSet[MAX_SET_SZ];
    EncodedAttrib unauthAttribs[MAX_UNAUTH_ATTRIBS_SZ];
    word32 unauthAttribsSz = 0, unauthAttribsCount = 0;
    word32 unauthAttribsSetSz = 0;


    PKCS7Attrib contentTypeAttrib;
    byte contentTypeValue[MAX_OID_SZ];
    /* contentType OID (1.2.840.113549.1.9.3) */
    const byte contentTypeOid[] =
            { ASN_OBJECT_ID, 0x09, 0x2a, 0x86, 0x48, 0x86, 0xF7, 0x0d, 0x01,
                             0x09, 0x03 };

    if (pkcs7 == NULL || pkcs7->content == NULL || pkcs7->contentSz == 0)
        return BAD_FUNC_ARG;

    if (output == NULL || outputSz == 0)
        return BAD_FUNC_ARG;

    if (pkcs7->encryptOID != AES128GCMb &&
        pkcs7->encryptOID != AES192GCMb &&
        pkcs7->encryptOID != AES256GCMb &&
        pkcs7->encryptOID != AES128CCMb &&
        pkcs7->encryptOID != AES192CCMb &&
        pkcs7->encryptOID != AES256CCMb) {
        WOLFSSL_MSG("CMS AuthEnvelopedData must use AES-GCM or AES-CCM");
        return BAD_FUNC_ARG;
    }

    blockKeySz = wc_PKCS7_GetOIDKeySize(pkcs7->encryptOID);
    if (blockKeySz < 0)
        return blockKeySz;

    blockSz = wc_PKCS7_GetOIDBlockSize(pkcs7->encryptOID);
    if (blockKeySz < 0 || blockSz < 0)
        return blockSz;

    /* outer content type */
    ret = wc_SetContentType(AUTH_ENVELOPED_DATA, outerContentType,
                            sizeof(outerContentType));
    if (ret < 0)
        return ret;

    outerContentTypeSz = ret;

    /* version, defined as 0 in RFC 5083 */
    verSz = SetMyVersion(0, ver, 0);

    /* generate random content encryption key */
    ret = PKCS7_GenerateContentEncryptionKey(pkcs7, blockKeySz);
    if (ret != 0) {
        return ret;
    }

    /* build RecipientInfo, only if user manually set singleCert and size */
    if (pkcs7->singleCert != NULL && pkcs7->singleCertSz > 0) {
        switch (pkcs7->publicKeyOID) {
        #ifndef NO_RSA
            case RSAk:
                ret = wc_PKCS7_AddRecipient_KTRI(pkcs7, pkcs7->singleCert,
                                                 pkcs7->singleCertSz, 0);
                break;
        #endif
        #ifdef HAVE_ECC
            case ECDSAk:
                ret = wc_PKCS7_AddRecipient_KARI(pkcs7, pkcs7->singleCert,
                                                 pkcs7->singleCertSz,
                                                 pkcs7->keyWrapOID,
                                                 pkcs7->keyAgreeOID, pkcs7->ukm,
                                                 pkcs7->ukmSz, 0);
                break;
        #endif

            default:
                WOLFSSL_MSG("Unsupported RecipientInfo public key type");
                return BAD_FUNC_ARG;
        };

        if (ret < 0) {
            WOLFSSL_MSG("Failed to create RecipientInfo");
            return ret;
        }
    }

    recipSz = wc_PKCS7_GetRecipientListSize(pkcs7);
    if (recipSz < 0) {
        return ret;

    } else if (recipSz == 0) {
        WOLFSSL_MSG("You must add at least one CMS recipient");
        return PKCS7_RECIP_E;
    }
    recipSetSz = SetSet(recipSz, recipSet);

    /* generate random nonce and IV for encryption */
    if (pkcs7->encryptOID == AES128GCMb ||
        pkcs7->encryptOID == AES192GCMb ||
        pkcs7->encryptOID == AES256GCMb) {
        /* GCM nonce is GCM_NONCE_MID_SZ (12) */
        nonceSz = GCM_NONCE_MID_SZ;
    } else {
        /* CCM nonce is CCM_NONCE_MIN_SZ (7) */
        nonceSz = CCM_NONCE_MIN_SZ;
    }

    ret = wc_InitRng_ex(&rng, pkcs7->heap, pkcs7->devId);
    if (ret != 0)
        return ret;

    ret = wc_PKCS7_GenerateBlock(pkcs7, &rng, nonce, nonceSz);
    wc_FreeRng(&rng);
    if (ret != 0) {
        return ret;
    }


    /* authAttribs: add contentType attrib if needed */
    if (pkcs7->contentOID != DATA) {

        /* if type is not id-data, contentType attribute MUST be added */
        contentTypeAttrib.oid = contentTypeOid;
        contentTypeAttrib.oidSz = sizeof(contentTypeOid);

        /* try to set from contentOID first, known types */
        ret = wc_SetContentType(pkcs7->contentOID, contentTypeValue,
                                sizeof(contentTypeValue));
        if (ret > 0) {
            contentTypeAttrib.value = contentTypeValue;
            contentTypeAttrib.valueSz = ret;

        /* otherwise, try to set from custom content type */
        } else if (ret <= 0) {
            if (pkcs7->contentType == NULL || pkcs7->contentTypeSz == 0) {
                WOLFSSL_MSG("CMS pkcs7->contentType must be set if "
                            "contentOID is not");
                return BAD_FUNC_ARG;
            }
            contentTypeAttrib.value = pkcs7->contentType;
            contentTypeAttrib.valueSz = pkcs7->contentTypeSz;
        }

        authAttribsSz += EncodeAttributes(authAttribs, 1,
                                          &contentTypeAttrib, 1);
        authAttribsCount += 1;
    }

    /* authAttribs: add in user authenticated attributes */
    if (pkcs7->authAttribs != NULL && pkcs7->authAttribsSz > 0) {
        authAttribsSz += EncodeAttributes(authAttribs + authAttribsCount,
                                 MAX_AUTH_ATTRIBS_SZ - authAttribsCount,
                                 pkcs7->authAttribs,
                                 pkcs7->authAttribsSz);
        authAttribsCount += pkcs7->authAttribsSz;
    }

    /* authAttribs: flatten authAttribs */
    if (authAttribsSz > 0 && authAttribsCount > 0) {
        flatAuthAttribs = (byte*)XMALLOC(authAttribsSz, pkcs7->heap,
                                         DYNAMIC_TYPE_PKCS7);
        if (flatAuthAttribs == NULL) {
            return MEMORY_E;
        }

        FlattenAttributes(flatAuthAttribs, authAttribs, authAttribsCount);

        authAttribsSetSz = SetImplicit(ASN_SET, 1, authAttribsSz,
                                       authAttribSet);

        /* From RFC5083, "For the purpose of constructing the AAD, the
         * IMPLICIT [1] tag in the authAttrs field is not used for the
         * DER encoding: rather a universal SET OF tag is used. */
        authAttribsAadSetSz = SetSet(authAttribsSz, authAttribAadSet);

        /* allocate temp buffer to hold alternate attrib encoding for aad */
        aadBuffer = (byte*)XMALLOC(authAttribsSz + authAttribsAadSetSz,
                                   pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
        if (aadBuffer == NULL) {
            XFREE(flatAuthAttribs, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
            return MEMORY_E;
        }

        /* build up alternate attrib encoding for aad */
        aadBufferSz = 0;
        XMEMCPY(aadBuffer + aadBufferSz, authAttribAadSet, authAttribsAadSetSz);
        aadBufferSz += authAttribsAadSetSz;
        XMEMCPY(aadBuffer + aadBufferSz, flatAuthAttribs, authAttribsSz);
        aadBufferSz += authAttribsSz;
    }

    /* build up unauthenticated attributes (unauthAttrs) */
    if (pkcs7->unauthAttribsSz > 0) {
        unauthAttribsSz = EncodeAttributes(unauthAttribs + unauthAttribsCount,
                                     MAX_UNAUTH_ATTRIBS_SZ - unauthAttribsCount,
                                     pkcs7->unauthAttribs,
                                     pkcs7->unauthAttribsSz);
        unauthAttribsCount = pkcs7->unauthAttribsSz;

        flatUnauthAttribs = (byte*)XMALLOC(unauthAttribsSz, pkcs7->heap,
                                            DYNAMIC_TYPE_PKCS7);
        if (flatUnauthAttribs == NULL) {
            if (aadBuffer)
                XFREE(aadBuffer, pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
            if (flatAuthAttribs)
                XFREE(flatAuthAttribs, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
            return MEMORY_E;
        }

        FlattenAttributes(flatUnauthAttribs, unauthAttribs, unauthAttribsCount);
        unauthAttribsSetSz = SetImplicit(ASN_SET, 2, unauthAttribsSz,
                                         unauthAttribSet);
    }

    /* allocate encrypted content buffer */
    encryptedOutSz = pkcs7->contentSz;
    encryptedContent = (byte*)XMALLOC(encryptedOutSz, pkcs7->heap,
                                      DYNAMIC_TYPE_PKCS7);
    if (encryptedContent == NULL) {
        if (aadBuffer)
            XFREE(aadBuffer, pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
        if (flatUnauthAttribs)
            XFREE(flatUnauthAttribs, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        if (flatAuthAttribs)
            XFREE(flatAuthAttribs, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        return MEMORY_E;
    }

    /* encrypt content */
    ret = wc_PKCS7_EncryptContent(pkcs7->encryptOID, pkcs7->cek,
            pkcs7->cekSz, nonce, nonceSz, aadBuffer, aadBufferSz, authTag,
            sizeof(authTag), pkcs7->content, encryptedOutSz, encryptedContent);

    if (aadBuffer) {
        XFREE(aadBuffer, pkcs7->heap, DYNAMIC_TYPE_TMP_BUFFER);
        aadBuffer = NULL;
    }

    if (ret != 0) {
        if (flatUnauthAttribs)
            XFREE(flatUnauthAttribs, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        if (flatAuthAttribs)
            XFREE(flatAuthAttribs, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        XFREE(encryptedContent, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        return ret;
    }

    /* EncryptedContentInfo */
    ret = wc_SetContentType(pkcs7->contentOID, contentType,
                            sizeof(contentType));
    if (ret < 0) {
        if (flatUnauthAttribs)
            XFREE(flatUnauthAttribs, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        if (flatAuthAttribs)
            XFREE(flatAuthAttribs, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        XFREE(encryptedContent, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        return ret;
    }

    contentTypeSz = ret;

    /* put together nonce OCTET STRING */
    nonceOctetStringSz = SetOctetString(nonceSz, nonceOctetString);

    /* put together aes-ICVlen INTEGER */
    macIntSz = SetMyVersion(sizeof(authTag), macInt, 0);

    /* build up our ContentEncryptionAlgorithmIdentifier sequence,
     * adding (nonceOctetStringSz + blockSz + macIntSz) for nonce OCTET STRING
     * and tag size */
    contentEncAlgoSz = SetAlgoID(pkcs7->encryptOID, contentEncAlgo,
                                 oidBlkType, nonceOctetStringSz + nonceSz +
                                 macIntSz);

    if (contentEncAlgoSz == 0) {
        if (flatUnauthAttribs)
            XFREE(flatUnauthAttribs, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        if (flatAuthAttribs)
            XFREE(flatAuthAttribs, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        XFREE(encryptedContent, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        return BAD_FUNC_ARG;
    }

    encContentOctetSz = SetImplicit(ASN_OCTET_STRING, 0, encryptedOutSz,
                                    encContentOctet);

    encContentSeqSz = SetSequence(contentTypeSz + contentEncAlgoSz +
                                  nonceOctetStringSz + nonceSz + macIntSz +
                                  encContentOctetSz + encryptedOutSz,
                                  encContentSeq);

    macOctetStringSz = SetOctetString(sizeof(authTag), macOctetString);

    /* keep track of sizes for outer wrapper layering */
    totalSz = verSz + recipSetSz + recipSz + encContentSeqSz + contentTypeSz +
              contentEncAlgoSz + nonceOctetStringSz + nonceSz + macIntSz +
              encContentOctetSz + encryptedOutSz + authAttribsSz +
              authAttribsSetSz + macOctetStringSz + sizeof(authTag) +
              unauthAttribsSz + unauthAttribsSetSz;

    /* EnvelopedData */
    envDataSeqSz = SetSequence(totalSz, envDataSeq);
    totalSz += envDataSeqSz;

    /* outer content */
    outerContentSz = SetExplicit(0, totalSz, outerContent);
    totalSz += outerContentTypeSz;
    totalSz += outerContentSz;

    /* ContentInfo */
    contentInfoSeqSz = SetSequence(totalSz, contentInfoSeq);
    totalSz += contentInfoSeqSz;

    if (totalSz > (int)outputSz) {
        WOLFSSL_MSG("Pkcs7_encrypt output buffer too small");
        if (flatUnauthAttribs)
            XFREE(flatUnauthAttribs, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        if (flatAuthAttribs)
            XFREE(flatAuthAttribs, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        XFREE(encryptedContent, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        return BUFFER_E;
    }

    XMEMCPY(output + idx, contentInfoSeq, contentInfoSeqSz);
    idx += contentInfoSeqSz;
    XMEMCPY(output + idx, outerContentType, outerContentTypeSz);
    idx += outerContentTypeSz;
    XMEMCPY(output + idx, outerContent, outerContentSz);
    idx += outerContentSz;
    XMEMCPY(output + idx, envDataSeq, envDataSeqSz);
    idx += envDataSeqSz;
    XMEMCPY(output + idx, ver, verSz);
    idx += verSz;
    XMEMCPY(output + idx, recipSet, recipSetSz);
    idx += recipSetSz;
    /* copy in recipients from list */
    tmpRecip = pkcs7->recipList;
    while (tmpRecip != NULL) {
        XMEMCPY(output + idx, tmpRecip->recip, tmpRecip->recipSz);
        idx += tmpRecip->recipSz;
        tmpRecip = tmpRecip->next;
    }
    wc_PKCS7_FreeEncodedRecipientSet(pkcs7);
    XMEMCPY(output + idx, encContentSeq, encContentSeqSz);
    idx += encContentSeqSz;
    XMEMCPY(output + idx, contentType, contentTypeSz);
    idx += contentTypeSz;
    XMEMCPY(output + idx, contentEncAlgo, contentEncAlgoSz);
    idx += contentEncAlgoSz;
    XMEMCPY(output + idx, nonceOctetString, nonceOctetStringSz);
    idx += nonceOctetStringSz;
    XMEMCPY(output + idx, nonce, nonceSz);
    idx += nonceSz;
    XMEMCPY(output + idx, macInt, macIntSz);
    idx += macIntSz;
    XMEMCPY(output + idx, encContentOctet, encContentOctetSz);
    idx += encContentOctetSz;
    XMEMCPY(output + idx, encryptedContent, encryptedOutSz);
    idx += encryptedOutSz;

    /* authenticated attributes */
    if (authAttribsSz > 0) {
        XMEMCPY(output + idx, authAttribSet, authAttribsSetSz);
        idx += authAttribsSetSz;
        XMEMCPY(output + idx, flatAuthAttribs, authAttribsSz);
        idx += authAttribsSz;
        XFREE(flatAuthAttribs, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
    }

    XMEMCPY(output + idx, macOctetString, macOctetStringSz);
    idx += macOctetStringSz;
    XMEMCPY(output + idx, authTag, sizeof(authTag));
    idx += sizeof(authTag);

    /* unauthenticated attributes */
    if (unauthAttribsSz > 0) {
        XMEMCPY(output + idx, unauthAttribSet, unauthAttribsSetSz);
        idx += unauthAttribsSetSz;
        XMEMCPY(output + idx, flatUnauthAttribs, unauthAttribsSz);
        idx += unauthAttribsSz;
        XFREE(flatUnauthAttribs, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
    }

    XFREE(encryptedContent, pkcs7->heap, DYNAMIC_TYPE_PKCS7);

    return idx;
}


/* unwrap and decrypt PKCS#7 AuthEnvelopedData object, return decoded size */
WOLFSSL_API int wc_PKCS7_DecodeAuthEnvelopedData(PKCS7* pkcs7, byte* in,
                                                 word32 inSz, byte* output,
                                                 word32 outputSz)
{
    int recipFound = 0;
    int ret = 0, length;
    word32 idx = 0;
#ifndef NO_PKCS7_STREAM
    word32 tmpIdx = 0;
#endif
    word32 contentType, encOID = 0;
    word32 decryptedKeySz = 0;
    byte* pkiMsg = in;
    word32 pkiMsgSz = inSz;

    int expBlockSz = 0, blockKeySz = 0;
    byte authTag[AES_BLOCK_SIZE];
    byte nonce[GCM_NONCE_MID_SZ];       /* GCM nonce is larger than CCM */
    int nonceSz = 0, authTagSz = 0, macSz = 0;

#ifdef WOLFSSL_SMALL_STACK
    byte* decryptedKey = NULL;
#else
    byte  decryptedKey[MAX_ENCRYPTED_KEY_SZ];
#endif
    int encryptedContentSz;
    byte* encryptedContent = NULL;
    int explicitOctet = 0;

    byte authAttribSetByte = 0;
    byte* encodedAttribs = NULL;
    word32 encodedAttribIdx = 0, encodedAttribSz = 0;
    byte* authAttrib = NULL;
    int authAttribSz = 0;

    if (pkcs7 == NULL)
        return BAD_FUNC_ARG;

    if (pkiMsg == NULL || pkiMsgSz == 0 ||
        output == NULL || outputSz == 0)
        return BAD_FUNC_ARG;
#ifndef NO_PKCS7_STREAM
    if (pkcs7->stream == NULL) {
        if ((ret = wc_PKCS7_CreateStream(pkcs7)) != 0) {
            return ret;
        }
    }
#endif

    switch (pkcs7->state) {
        case WC_PKCS7_START:
        case WC_PKCS7_INFOSET_START:
        case WC_PKCS7_INFOSET_STAGE1:
        case WC_PKCS7_INFOSET_STAGE2:
        case WC_PKCS7_INFOSET_END:
            ret = wc_PKCS7_ParseToRecipientInfoSet(pkcs7, pkiMsg, pkiMsgSz,
                    &idx, AUTH_ENVELOPED_DATA);
            if (ret < 0)
                break;

        #ifndef NO_PKCS7_STREAM
            tmpIdx = idx;
        #endif
            wc_PKCS7_ChangeState(pkcs7, WC_PKCS7_AUTHENV_2);
            FALL_THROUGH;

        case WC_PKCS7_AUTHENV_2:
        #ifndef NO_PKCS7_STREAM
            if ((ret = wc_PKCS7_AddDataToStream(pkcs7, in, inSz, MAX_LENGTH_SZ +
                            MAX_VERSION_SZ + ASN_TAG_SZ, &pkiMsg, &idx)) != 0) {
                break;
            }
        #endif
        #ifdef WOLFSSL_SMALL_STACK
            decryptedKey = (byte*)XMALLOC(MAX_ENCRYPTED_KEY_SZ, pkcs7->heap,
                                                               DYNAMIC_TYPE_PKCS7);
            if (decryptedKey == NULL) {
                ret = MEMORY_E;
                break;
            }
        #ifndef NO_PKCS7_STREAM
            pkcs7->stream->key = decryptedKey;
        #endif
        #endif
            FALL_THROUGH;

        case WC_PKCS7_DECRYPT_KTRI:
        case WC_PKCS7_DECRYPT_KTRI_2:
        case WC_PKCS7_DECRYPT_KTRI_3:
        case WC_PKCS7_DECRYPT_KARI:
        case WC_PKCS7_DECRYPT_KEKRI:
        case WC_PKCS7_DECRYPT_PWRI:
        case WC_PKCS7_DECRYPT_ORI:

            decryptedKeySz = MAX_ENCRYPTED_KEY_SZ;
        #ifdef WOLFSSL_SMALL_STACK
            #ifndef NO_PKCS7_STREAM
            decryptedKey = pkcs7->stream->key;
            #endif
        #endif

            ret = wc_PKCS7_DecryptRecipientInfos(pkcs7, in, inSz, &idx,
                                                decryptedKey, &decryptedKeySz,
                                                &recipFound);
            if (ret != 0) {
                break;
            }

            if (recipFound == 0) {
                WOLFSSL_MSG("No recipient found in envelopedData that matches input");
                ret = PKCS7_RECIP_E;
                break;
            }

        #ifndef NO_PKCS7_STREAM
            tmpIdx = idx;
        #endif
            wc_PKCS7_ChangeState(pkcs7, WC_PKCS7_AUTHENV_3);
            FALL_THROUGH;

        case WC_PKCS7_AUTHENV_3:
        #ifndef NO_PKCS7_STREAM
            if ((ret = wc_PKCS7_AddDataToStream(pkcs7, in, inSz, MAX_SEQ_SZ +
                            MAX_ALGO_SZ + MAX_ALGO_SZ + ASN_TAG_SZ,
                            &pkiMsg, &idx)) != 0) {
                break;
            }

            pkiMsgSz = wc_PKCS7_GetMaxStream(pkcs7, PKCS7_DEFAULT_PEEK,
                    in, inSz);
        #endif

            /* remove EncryptedContentInfo */
            if (ret == 0 && GetSequence(pkiMsg, &idx, &length, pkiMsgSz) < 0) {
                ret = ASN_PARSE_E;
            }

            if (ret == 0 && wc_GetContentType(pkiMsg, &idx, &contentType,
                        pkiMsgSz) < 0) {
                ret = ASN_PARSE_E;
            }

            if (ret == 0 && GetAlgoId(pkiMsg, &idx, &encOID, oidBlkType,
                        pkiMsgSz) < 0) {
                ret = ASN_PARSE_E;
            }

            blockKeySz = wc_PKCS7_GetOIDKeySize(encOID);
            if (ret == 0 && blockKeySz < 0) {
                ret = blockKeySz;
            }

            expBlockSz = wc_PKCS7_GetOIDBlockSize(encOID);
            if (ret == 0 && expBlockSz < 0) {
                ret = expBlockSz;
            }

            /* get nonce, stored in OPTIONAL parameter of AlgoID */
            if (ret == 0 && pkiMsg[idx++] != ASN_OCTET_STRING) {
                ret = ASN_PARSE_E;
            }

            if (ret < 0)
                break;

        #ifndef NO_PKCS7_STREAM
            if ((ret = wc_PKCS7_StreamEndCase(pkcs7, &tmpIdx, &idx)) != 0) {
                break;
            }
            wc_PKCS7_StreamStoreVar(pkcs7, encOID, blockKeySz, 0);
        #endif
            wc_PKCS7_ChangeState(pkcs7, WC_PKCS7_AUTHENV_4);
            FALL_THROUGH;

        case WC_PKCS7_AUTHENV_4:

        #ifndef NO_PKCS7_STREAM
            if ((ret = wc_PKCS7_AddDataToStream(pkcs7, in, inSz, MAX_LENGTH_SZ +
                            MAX_VERSION_SZ + ASN_TAG_SZ + MAX_LENGTH_SZ,
                            &pkiMsg, &idx)) != 0) {
                break;
            }

            pkiMsgSz = wc_PKCS7_GetMaxStream(pkcs7, PKCS7_SEQ_PEEK, in, inSz);
        #endif
            if (ret == 0 && GetLength(pkiMsg, &idx, &nonceSz, pkiMsgSz) < 0) {
                ret = ASN_PARSE_E;
            }

            if (ret == 0 && nonceSz > (int)sizeof(nonce)) {
                WOLFSSL_MSG("AuthEnvelopedData nonce too large for buffer");
                ret = ASN_PARSE_E;
            }

            if (ret == 0) {
                XMEMCPY(nonce, &pkiMsg[idx], nonceSz);
                idx += nonceSz;
            }

            /* get mac size, also stored in OPTIONAL parameter of AlgoID */
            if (ret == 0 && GetMyVersion(pkiMsg, &idx, &macSz, pkiMsgSz) < 0) {
                ret = ASN_PARSE_E;
            }

            if (ret == 0) {
                explicitOctet = pkiMsg[idx] ==
                    (ASN_CONTEXT_SPECIFIC | ASN_CONSTRUCTED | 0);
            }

            /* read encryptedContent, cont[0] */
            if (ret == 0 && pkiMsg[idx] != (ASN_CONTEXT_SPECIFIC | 0) &&
                pkiMsg[idx] != (ASN_CONTEXT_SPECIFIC | ASN_CONSTRUCTED | 0)) {
                ret = ASN_PARSE_E;
            }
            idx++;

            if (ret == 0 && GetLength(pkiMsg, &idx, &encryptedContentSz,
                        pkiMsgSz) <= 0) {
                ret = ASN_PARSE_E;
            }

            if (explicitOctet) {
                if (ret == 0 && pkiMsg[idx++] != ASN_OCTET_STRING) {
                    ret = ASN_PARSE_E;
                }

                if (ret == 0 && GetLength(pkiMsg, &idx, &encryptedContentSz,
                            pkiMsgSz) <= 0) {
                    ret = ASN_PARSE_E;
                }
            }

            if (ret < 0)
                break;

        #ifndef NO_PKCS7_STREAM
            if ((ret = wc_PKCS7_StreamEndCase(pkcs7, &tmpIdx, &idx)) != 0) {
                break;
            }

            /* store nonce for later */
            if (nonceSz > 0) {
                pkcs7->stream->nonceSz = nonceSz;
                pkcs7->stream->nonce = (byte*)XMALLOC(nonceSz, pkcs7->heap,
                        DYNAMIC_TYPE_PKCS7);
                if (pkcs7->stream->nonce == NULL) {
                    ret = MEMORY_E;
                    break;
                }
                else {
                    XMEMCPY(pkcs7->stream->nonce, nonce, nonceSz);
                }
            }

            pkcs7->stream->expected = encryptedContentSz;
            wc_PKCS7_StreamStoreVar(pkcs7, encOID, blockKeySz,
                    encryptedContentSz);
        #endif

            wc_PKCS7_ChangeState(pkcs7, WC_PKCS7_AUTHENV_5);
            FALL_THROUGH;

        case WC_PKCS7_AUTHENV_5:
        #ifndef NO_PKCS7_STREAM
            if ((ret = wc_PKCS7_AddDataToStream(pkcs7, in, inSz, MAX_LENGTH_SZ +
                            ASN_TAG_SZ + ASN_TAG_SZ + pkcs7->stream->expected,
                            &pkiMsg, &idx)) != 0) {
                break;
            }

            pkiMsgSz = wc_PKCS7_GetMaxStream(pkcs7, PKCS7_DEFAULT_PEEK, in, inSz);
            encryptedContentSz = pkcs7->stream->expected;
        #endif

            encryptedContent = (byte*)XMALLOC(encryptedContentSz, pkcs7->heap,
                                                               DYNAMIC_TYPE_PKCS7);
            if (ret == 0 && encryptedContent == NULL) {
                ret = MEMORY_E;
            }

            if (ret == 0) {
                XMEMCPY(encryptedContent, &pkiMsg[idx], encryptedContentSz);
                idx += encryptedContentSz;
            }
        #ifndef NO_PKCS7_STREAM
                pkcs7->stream->bufferPt = encryptedContent;
        #endif

            /* may have IMPLICIT [1] authenticatedAttributes */
            if (ret == 0 && pkiMsg[idx] ==
                    (ASN_CONSTRUCTED | ASN_CONTEXT_SPECIFIC | 1)) {
                encodedAttribIdx = idx;
                encodedAttribs = pkiMsg + idx;
                idx++;

                if (ret == 0 && GetLength(pkiMsg, &idx, &length, pkiMsgSz) < 0)
                    ret = ASN_PARSE_E;
            #ifndef NO_PKCS7_STREAM
                pkcs7->stream->expected = length;
            #endif
                encodedAttribSz = length + (idx - encodedAttribIdx);

                if (ret != 0)
                    break;

            #ifndef NO_PKCS7_STREAM
                if (encodedAttribSz > 0) {
                    pkcs7->stream->aadSz = encodedAttribSz;
                    pkcs7->stream->aad = (byte*)XMALLOC(encodedAttribSz,
                            pkcs7->heap, DYNAMIC_TYPE_PKCS7);
                    if (pkcs7->stream->aad == NULL) {
                        ret = MEMORY_E;
                        break;
                    }
                    else {
                        XMEMCPY(pkcs7->stream->aad, encodedAttribs,
                                (idx - encodedAttribIdx));
                    }
                }

                if ((ret = wc_PKCS7_StreamEndCase(pkcs7, &tmpIdx, &idx)) != 0) {
                    break;
                }
            #endif
                wc_PKCS7_ChangeState(pkcs7, WC_PKCS7_AUTHENV_ATRB);
            }
            else {
            #ifndef NO_PKCS7_STREAM
                if ((ret = wc_PKCS7_StreamEndCase(pkcs7, &tmpIdx, &idx)) != 0) {
                    break;
                }
            #endif
                goto authenv_atrbend; /* jump over attribute cases */
            }
            FALL_THROUGH;

        case WC_PKCS7_AUTHENV_ATRB:
    #ifndef NO_PKCS7_STREAM
            if ((ret = wc_PKCS7_AddDataToStream(pkcs7, in, inSz,
                            pkcs7->stream->expected, &pkiMsg, &idx)) != 0) {
                return ret;
            }

            length = pkcs7->stream->expected;
            encodedAttribs = pkcs7->stream->aad;
    #endif

            /* save pointer and length */
            authAttrib = &pkiMsg[idx];
            authAttribSz = length;

            if (ret == 0 && wc_PKCS7_ParseAttribs(pkcs7, authAttrib, authAttribSz) < 0) {
                WOLFSSL_MSG("Error parsing authenticated attributes");
                ret = ASN_PARSE_E;
                break;
            }

            idx += length;

    #ifndef NO_PKCS7_STREAM
            if (encodedAttribSz > 0) {
                XMEMCPY(pkcs7->stream->aad + (encodedAttribSz - length),
                        authAttrib, authAttribSz);
            }
            if ((ret = wc_PKCS7_StreamEndCase(pkcs7, &tmpIdx, &idx)) != 0) {
                break;
            }

    #endif
            wc_PKCS7_ChangeState(pkcs7, WC_PKCS7_AUTHENV_ATRBEND);
            FALL_THROUGH;

authenv_atrbend:
        case WC_PKCS7_AUTHENV_ATRBEND:
        #ifndef NO_PKCS7_STREAM
            if ((ret = wc_PKCS7_AddDataToStream(pkcs7, in, inSz, MAX_LENGTH_SZ +
                            ASN_TAG_SZ, &pkiMsg, &idx)) != 0) {
                return ret;
            }

            pkiMsgSz = wc_PKCS7_GetMaxStream(pkcs7, PKCS7_DEFAULT_PEEK,
                    in, inSz);
            if (pkcs7->stream->aadSz > 0) {
                encodedAttribSz = pkcs7->stream->aadSz;
                encodedAttribs  = pkcs7->stream->aad;
            }
        #endif


            /* get authTag OCTET STRING */
            if (ret == 0 && pkiMsg[idx++] != ASN_OCTET_STRING) {
                ret = ASN_PARSE_E;
            }

            if (ret == 0 && GetLength(pkiMsg, &idx, &authTagSz, pkiMsgSz) < 0) {
                ret = ASN_PARSE_E;
            }

            if (ret == 0 && authTagSz > (int)sizeof(authTag)) {
                WOLFSSL_MSG("AuthEnvelopedData authTag too large for buffer");
                ret = ASN_PARSE_E;
            }

            if (ret == 0) {
                XMEMCPY(authTag, &pkiMsg[idx], authTagSz);
                idx += authTagSz;
            }

            if (ret == 0 && authAttrib != NULL) {
                /* temporarily swap authAttribs byte[0] to SET OF instead of
                 * IMPLICIT [1], for aad calculation */
                authAttribSetByte = encodedAttribs[0];

                encodedAttribs[0] = ASN_SET | ASN_CONSTRUCTED;
            }

            if (ret < 0)
                break;

        #ifndef NO_PKCS7_STREAM
            if ((ret = wc_PKCS7_StreamEndCase(pkcs7, &tmpIdx, &idx)) != 0) {
                break;
            }
            pkcs7->stream->expected = (pkcs7->stream->maxLen -
                pkcs7->stream->totalRd) + pkcs7->stream->length;


            /* store tag for later */
            if (authTagSz > 0) {
                pkcs7->stream->tagSz = authTagSz;
                pkcs7->stream->tag = (byte*)XMALLOC(authTagSz, pkcs7->heap,
                        DYNAMIC_TYPE_PKCS7);
                if (pkcs7->stream->tag == NULL) {
                    ret = MEMORY_E;
                    break;
                }
                else {
                    XMEMCPY(pkcs7->stream->tag, authTag, authTagSz);
                }
            }

        #endif
            wc_PKCS7_ChangeState(pkcs7, WC_PKCS7_AUTHENV_6);
            FALL_THROUGH;

        case WC_PKCS7_AUTHENV_6:
        #ifndef NO_PKCS7_STREAM
            if ((ret = wc_PKCS7_AddDataToStream(pkcs7, in, inSz,
                            pkcs7->stream->expected, &pkiMsg, &idx)) != 0) {
                break;
            }

            /* restore all variables needed */
            if (pkcs7->stream->nonceSz > 0) {
                nonceSz = pkcs7->stream->nonceSz;
                if (nonceSz > GCM_NONCE_MID_SZ) {
                    WOLFSSL_MSG("PKCS7 saved nonce is too large");
                    ret = BUFFER_E;
                    break;
                }
                else {
                    XMEMCPY(nonce, pkcs7->stream->nonce, nonceSz);
                }
            }

            if (pkcs7->stream->tagSz > 0) {
                authTagSz = pkcs7->stream->tagSz;
                if (authTagSz > AES_BLOCK_SIZE) {
                    WOLFSSL_MSG("PKCS7 saved tag is too large");
                    ret = BUFFER_E;
                    break;
                }
                else {
                    XMEMCPY(authTag, pkcs7->stream->tag, authTagSz);
                }
            }

            if (pkcs7->stream->aadSz > 0) {
                encodedAttribSz = pkcs7->stream->aadSz;
                encodedAttribs  = pkcs7->stream->aad;
            }

            wc_PKCS7_StreamGetVar(pkcs7, &encOID, &blockKeySz, &encryptedContentSz);
            encryptedContent   = pkcs7->stream->bufferPt;
        #ifdef WOLFSSL_SMALL_STACK
            decryptedKey = pkcs7->stream->key;
        #endif
        #endif

            /* decrypt encryptedContent */
            ret = wc_PKCS7_DecryptContent(encOID, decryptedKey, blockKeySz,
                                          nonce, nonceSz, encodedAttribs,
                                          encodedAttribSz, authTag, authTagSz,
                                          encryptedContent, encryptedContentSz,
                                          encryptedContent);
            if (ret != 0) {
                XFREE(encryptedContent, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
                return ret;
            }

            if (authAttrib != NULL) {
                /* restore authAttrib IMPLICIT [1] */
                encodedAttribs[0] = authAttribSetByte;
            }

            /* copy plaintext to output */
            XMEMCPY(output, encryptedContent, encryptedContentSz);

            /* free memory, zero out keys */
            ForceZero(encryptedContent, encryptedContentSz);
            XFREE(encryptedContent, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
            ForceZero(decryptedKey, MAX_ENCRYPTED_KEY_SZ);
        #ifdef WOLFSSL_SMALL_STACK
            XFREE(decryptedKey, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
            decryptedKey = NULL;
        #ifdef WOLFSSL_SMALL_STACK
            pkcs7->stream->key = NULL;
        #endif
        #endif
            ret = encryptedContentSz;
        #ifndef NO_PKCS7_STREAM
            wc_PKCS7_ResetStream(pkcs7);
        #endif
            wc_PKCS7_ChangeState(pkcs7, WC_PKCS7_START);
            break;
        default:
            WOLFSSL_MSG("Unknown PKCS7 state");
            ret = BAD_FUNC_ARG;
    }

#ifdef WOLFSSL_SMALL_STACK
    if (ret != 0 && ret != WC_PKCS7_WANT_READ_E) {
        if (decryptedKey != NULL) {
            ForceZero(decryptedKey, MAX_ENCRYPTED_KEY_SZ);
        }
        XFREE(decryptedKey, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
    }
#endif
#ifndef NO_PKCS7_STREAM
    if (ret != 0 && ret != WC_PKCS7_WANT_READ_E) {
        wc_PKCS7_ResetStream(pkcs7);
    }
#endif
    return ret;
}


#ifndef NO_PKCS7_ENCRYPTED_DATA

/* build PKCS#7 encryptedData content type, return encrypted size */
int wc_PKCS7_EncodeEncryptedData(PKCS7* pkcs7, byte* output, word32 outputSz)
{
    int ret, idx = 0;
    int totalSz, padSz, encryptedOutSz;

    int contentInfoSeqSz, outerContentTypeSz, outerContentSz;
    byte contentInfoSeq[MAX_SEQ_SZ];
    byte outerContentType[MAX_ALGO_SZ];
    byte outerContent[MAX_SEQ_SZ];

    int encDataSeqSz, verSz, blockSz;
    byte encDataSeq[MAX_SEQ_SZ];
    byte ver[MAX_VERSION_SZ];

    byte* plain = NULL;
    byte* encryptedContent = NULL;

    int encContentOctetSz, encContentSeqSz, contentTypeSz;
    int contentEncAlgoSz, ivOctetStringSz;
    byte encContentSeq[MAX_SEQ_SZ];
    byte contentType[MAX_OID_SZ];
    byte contentEncAlgo[MAX_ALGO_SZ];
    byte tmpIv[MAX_CONTENT_IV_SIZE];
    byte ivOctetString[MAX_OCTET_STR_SZ];
    byte encContentOctet[MAX_OCTET_STR_SZ];

    byte attribSet[MAX_SET_SZ];
    EncodedAttrib* attribs = NULL;
    word32 attribsSz;
    word32 attribsCount;
    word32 attribsSetSz;

    byte* flatAttribs = NULL;

    if (pkcs7 == NULL || pkcs7->content == NULL || pkcs7->contentSz == 0 ||
        pkcs7->encryptOID == 0 || pkcs7->encryptionKey == NULL ||
        pkcs7->encryptionKeySz == 0)
        return BAD_FUNC_ARG;

    if (output == NULL || outputSz == 0)
        return BAD_FUNC_ARG;

    /* outer content type */
    ret = wc_SetContentType(ENCRYPTED_DATA, outerContentType,
                            sizeof(outerContentType));
    if (ret < 0)
        return ret;

    outerContentTypeSz = ret;

    /* version, 2 if unprotectedAttrs present, 0 if absent */
    if (pkcs7->unprotectedAttribsSz > 0) {
        verSz = SetMyVersion(2, ver, 0);
    } else {
        verSz = SetMyVersion(0, ver, 0);
    }

    /* EncryptedContentInfo */
    ret = wc_SetContentType(pkcs7->contentOID, contentType,
                            sizeof(contentType));
    if (ret < 0)
        return ret;

    contentTypeSz = ret;

    /* allocate encrypted content buffer, do PKCS#7 padding */
    blockSz = wc_PKCS7_GetOIDBlockSize(pkcs7->encryptOID);
    if (blockSz < 0)
        return blockSz;

    padSz = wc_PKCS7_GetPadSize(pkcs7->contentSz, blockSz);
    if (padSz < 0)
        return padSz;

    encryptedOutSz = pkcs7->contentSz + padSz;

    plain = (byte*)XMALLOC(encryptedOutSz, pkcs7->heap,
                           DYNAMIC_TYPE_PKCS7);
    if (plain == NULL)
        return MEMORY_E;

    ret = wc_PKCS7_PadData(pkcs7->content, pkcs7->contentSz, plain,
                           encryptedOutSz, blockSz);
    if (ret < 0) {
        XFREE(plain, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        return ret;
    }

    encryptedContent = (byte*)XMALLOC(encryptedOutSz, pkcs7->heap,
                                      DYNAMIC_TYPE_PKCS7);
    if (encryptedContent == NULL) {
        XFREE(plain, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        return MEMORY_E;
    }

    /* put together IV OCTET STRING */
    ivOctetStringSz = SetOctetString(blockSz, ivOctetString);

    /* build up ContentEncryptionAlgorithmIdentifier sequence,
       adding (ivOctetStringSz + blockSz) for IV OCTET STRING */
    contentEncAlgoSz = SetAlgoID(pkcs7->encryptOID, contentEncAlgo,
                                 oidBlkType, ivOctetStringSz + blockSz);
    if (contentEncAlgoSz == 0) {
        XFREE(encryptedContent, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        XFREE(plain, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        return BAD_FUNC_ARG;
    }

    /* encrypt content */
    ret = wc_PKCS7_GenerateBlock(pkcs7, NULL, tmpIv, blockSz);
    if (ret != 0) {
        XFREE(encryptedContent, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        XFREE(plain, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        return ret;
    }

    ret = wc_PKCS7_EncryptContent(pkcs7->encryptOID, pkcs7->encryptionKey,
            pkcs7->encryptionKeySz, tmpIv, blockSz, NULL, 0, NULL, 0,
            plain, encryptedOutSz, encryptedContent);
    if (ret != 0) {
        XFREE(encryptedContent, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        XFREE(plain, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        return ret;
    }

    encContentOctetSz = SetImplicit(ASN_OCTET_STRING, 0,
                                    encryptedOutSz, encContentOctet);

    encContentSeqSz = SetSequence(contentTypeSz + contentEncAlgoSz +
                                  ivOctetStringSz + blockSz +
                                  encContentOctetSz + encryptedOutSz,
                                  encContentSeq);

    /* optional UnprotectedAttributes */
    if (pkcs7->unprotectedAttribsSz != 0) {

        if (pkcs7->unprotectedAttribs == NULL) {
            XFREE(encryptedContent, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
            XFREE(plain, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
            return BAD_FUNC_ARG;
        }

        attribs = (EncodedAttrib*)XMALLOC(
                sizeof(EncodedAttrib) * pkcs7->unprotectedAttribsSz,
                pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        if (attribs == NULL) {
            XFREE(encryptedContent, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
            XFREE(plain, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
            return MEMORY_E;
        }

        attribsCount = pkcs7->unprotectedAttribsSz;
        attribsSz = EncodeAttributes(attribs, pkcs7->unprotectedAttribsSz,
                                     pkcs7->unprotectedAttribs,
                                     pkcs7->unprotectedAttribsSz);

        flatAttribs = (byte*)XMALLOC(attribsSz, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        if (flatAttribs == NULL) {
            XFREE(attribs, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
            XFREE(encryptedContent, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
            XFREE(plain, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
            return MEMORY_E;
        }

        FlattenAttributes(flatAttribs, attribs, attribsCount);
        attribsSetSz = SetImplicit(ASN_SET, 1, attribsSz, attribSet);

    } else {
        attribsSz = 0;
        attribsSetSz = 0;
    }

    /* keep track of sizes for outer wrapper layering */
    totalSz = verSz + encContentSeqSz + contentTypeSz + contentEncAlgoSz +
              ivOctetStringSz + blockSz + encContentOctetSz + encryptedOutSz +
              attribsSz + attribsSetSz;

    /* EncryptedData */
    encDataSeqSz = SetSequence(totalSz, encDataSeq);
    totalSz += encDataSeqSz;

    /* outer content */
    outerContentSz = SetExplicit(0, totalSz, outerContent);
    totalSz += outerContentTypeSz;
    totalSz += outerContentSz;

    /* ContentInfo */
    contentInfoSeqSz = SetSequence(totalSz, contentInfoSeq);
    totalSz += contentInfoSeqSz;

    if (totalSz > (int)outputSz) {
        WOLFSSL_MSG("PKCS#7 output buffer too small");
        if (pkcs7->unprotectedAttribsSz != 0) {
            XFREE(attribs, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
            XFREE(flatAttribs, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        }
        XFREE(encryptedContent, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        XFREE(plain, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        return BUFFER_E;
    }

    XMEMCPY(output + idx, contentInfoSeq, contentInfoSeqSz);
    idx += contentInfoSeqSz;
    XMEMCPY(output + idx, outerContentType, outerContentTypeSz);
    idx += outerContentTypeSz;
    XMEMCPY(output + idx, outerContent, outerContentSz);
    idx += outerContentSz;
    XMEMCPY(output + idx, encDataSeq, encDataSeqSz);
    idx += encDataSeqSz;
    XMEMCPY(output + idx, ver, verSz);
    idx += verSz;
    XMEMCPY(output + idx, encContentSeq, encContentSeqSz);
    idx += encContentSeqSz;
    XMEMCPY(output + idx, contentType, contentTypeSz);
    idx += contentTypeSz;
    XMEMCPY(output + idx, contentEncAlgo, contentEncAlgoSz);
    idx += contentEncAlgoSz;
    XMEMCPY(output + idx, ivOctetString, ivOctetStringSz);
    idx += ivOctetStringSz;
    XMEMCPY(output + idx, tmpIv, blockSz);
    idx += blockSz;
    XMEMCPY(output + idx, encContentOctet, encContentOctetSz);
    idx += encContentOctetSz;
    XMEMCPY(output + idx, encryptedContent, encryptedOutSz);
    idx += encryptedOutSz;

    if (pkcs7->unprotectedAttribsSz != 0) {
        XMEMCPY(output + idx, attribSet, attribsSetSz);
        idx += attribsSetSz;
        XMEMCPY(output + idx, flatAttribs, attribsSz);
        idx += attribsSz;
        XFREE(attribs, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        XFREE(flatAttribs, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
    }

    XFREE(encryptedContent, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
    XFREE(plain, pkcs7->heap, DYNAMIC_TYPE_PKCS7);

    return idx;
}


/* decode and store unprotected attributes in PKCS7->decodedAttrib. Return
 * 0 on success, negative on error. User must call wc_PKCS7_Free(). */
static int wc_PKCS7_DecodeUnprotectedAttributes(PKCS7* pkcs7, byte* pkiMsg,
                                             word32 pkiMsgSz, word32* inOutIdx)
{
    int ret, attribLen;
    word32 idx;

    if (pkcs7 == NULL || pkiMsg == NULL ||
        pkiMsgSz == 0 || inOutIdx == NULL)
        return BAD_FUNC_ARG;

    idx = *inOutIdx;

    if (pkiMsg[idx] != (ASN_CONSTRUCTED | ASN_CONTEXT_SPECIFIC | 1))
        return ASN_PARSE_E;
    idx++;

    if (GetLength(pkiMsg, &idx, &attribLen, pkiMsgSz) < 0)
        return ASN_PARSE_E;

    /* loop through attributes */
    if ((ret = wc_PKCS7_ParseAttribs(pkcs7, pkiMsg + idx, attribLen)) < 0) {
        return ret;
    }

    *inOutIdx = idx;

    return 0;
}


/* unwrap and decrypt PKCS#7/CMS encrypted-data object, returned decoded size */
int wc_PKCS7_DecodeEncryptedData(PKCS7* pkcs7, byte* in, word32 inSz,
                                 byte* output, word32 outputSz)
{
    int ret = 0, version, length, haveAttribs = 0;
    word32 idx = 0;

#ifndef NO_PKCS7_STREAM
    word32 tmpIdx = 0;
#endif
    word32 contentType, encOID;

    int expBlockSz = 0;
    byte tmpIvBuf[MAX_CONTENT_IV_SIZE];
    byte *tmpIv = tmpIvBuf;

    int encryptedContentSz = 0;
    byte padLen;
    byte* encryptedContent = NULL;

    byte* pkiMsg = in;
    word32 pkiMsgSz = inSz;

    if (pkcs7 == NULL || pkcs7->encryptionKey == NULL ||
        pkcs7->encryptionKeySz == 0)
        return BAD_FUNC_ARG;

    if (pkiMsg == NULL || pkiMsgSz == 0 ||
        output == NULL || outputSz == 0)
        return BAD_FUNC_ARG;

#ifndef NO_PKCS7_STREAM
    (void)tmpIv; /* help out static analysis */
    if (pkcs7->stream == NULL) {
        if ((ret = wc_PKCS7_CreateStream(pkcs7)) != 0) {
            return ret;
        }
    }
#endif

    switch (pkcs7->state) {
        case WC_PKCS7_START:
#ifndef NO_PKCS7_STREAM
            if ((ret = wc_PKCS7_AddDataToStream(pkcs7, in, inSz, MAX_SEQ_SZ +
                            MAX_ALGO_SZ, &pkiMsg, &idx)) != 0) {
                return ret;
            }

            pkiMsgSz = wc_PKCS7_GetMaxStream(pkcs7, PKCS7_SEQ_PEEK, in, inSz);
#endif

            /* read past ContentInfo, verify type is encrypted-data */
            if (GetSequence(pkiMsg, &idx, &length, pkiMsgSz) < 0)
                ret = ASN_PARSE_E;

            if (ret == 0 && wc_GetContentType(pkiMsg, &idx, &contentType,
                        pkiMsgSz) < 0)
                ret = ASN_PARSE_E;

            if (ret == 0 && contentType != ENCRYPTED_DATA) {
                WOLFSSL_MSG("PKCS#7 input not of type EncryptedData");
                ret = PKCS7_OID_E;
            }

            if (ret != 0) break;
#ifndef NO_PKCS7_STREAM
            if ((ret = wc_PKCS7_StreamEndCase(pkcs7, &tmpIdx, &idx)) != 0) {
                break;
            }
#endif
            wc_PKCS7_ChangeState(pkcs7, WC_PKCS7_STAGE2);
            FALL_THROUGH;
            /* end of stage 1 */

        case WC_PKCS7_STAGE2:
#ifndef NO_PKCS7_STREAM
            if ((ret = wc_PKCS7_AddDataToStream(pkcs7, in, inSz,
                            MAX_LENGTH_SZ + MAX_SEQ_SZ + ASN_TAG_SZ, &pkiMsg,
                            &idx)) != 0) {
                return ret;
            }

            pkiMsgSz = wc_PKCS7_GetMaxStream(pkcs7, PKCS7_DEFAULT_PEEK, in, inSz);
#endif
            if (ret == 0 && pkiMsg[idx++] != (ASN_CONSTRUCTED |
                        ASN_CONTEXT_SPECIFIC | 0))
                ret = ASN_PARSE_E;

            if (ret == 0 && GetLength(pkiMsg, &idx, &length, pkiMsgSz) < 0)
                ret = ASN_PARSE_E;

            /* remove EncryptedData and version */
            if (ret == 0 && GetSequence(pkiMsg, &idx, &length, pkiMsgSz) < 0)
                ret = ASN_PARSE_E;

            if (ret != 0) break;
#ifndef NO_PKCS7_STREAM
            if ((ret = wc_PKCS7_StreamEndCase(pkcs7, &tmpIdx, &idx)) != 0) {
                break;
            }
#endif
            wc_PKCS7_ChangeState(pkcs7, WC_PKCS7_STAGE3);
            FALL_THROUGH;
            /* end of stage 2 */

       case WC_PKCS7_STAGE3:
#ifndef NO_PKCS7_STREAM
            if ((ret = wc_PKCS7_AddDataToStream(pkcs7, in, inSz,
                            MAX_VERSION_SZ + MAX_SEQ_SZ + MAX_ALGO_SZ * 2,
                            &pkiMsg, &idx)) != 0) {
                return ret;
            }

            pkiMsgSz = wc_PKCS7_GetMaxStream(pkcs7, PKCS7_DEFAULT_PEEK, in, inSz);
#endif
            /* get version, check later */
            haveAttribs = 0;
            if (ret == 0 && GetMyVersion(pkiMsg, &idx, &version, pkiMsgSz) < 0)
                ret = ASN_PARSE_E;

            /* remove EncryptedContentInfo */
            if (ret == 0 && GetSequence(pkiMsg, &idx, &length, pkiMsgSz) < 0)
                ret = ASN_PARSE_E;

            if (ret == 0 && wc_GetContentType(pkiMsg, &idx, &contentType,
                        pkiMsgSz) < 0)
                ret = ASN_PARSE_E;

            if (ret == 0 && (ret = GetAlgoId(pkiMsg, &idx, &encOID, oidBlkType,
                        pkiMsgSz)) < 0)
                ret = ASN_PARSE_E;
            if (ret == 0 && (expBlockSz = wc_PKCS7_GetOIDBlockSize(encOID)) < 0)
                ret = expBlockSz;

            if (ret != 0) break;
#ifndef NO_PKCS7_STREAM
            /* store expBlockSz for later */
            pkcs7->stream->varOne = expBlockSz;
            pkcs7->stream->varTwo = encOID;

            if ((ret = wc_PKCS7_StreamEndCase(pkcs7, &tmpIdx, &idx)) != 0) {
                break;
            }

            /* store version for later */
            pkcs7->stream->vers = version;
#endif
            wc_PKCS7_ChangeState(pkcs7, WC_PKCS7_STAGE4);
            FALL_THROUGH;
            /* end of stage 3 */

        /* get block cipher IV, stored in OPTIONAL parameter of AlgoID */
       case WC_PKCS7_STAGE4:
#ifndef NO_PKCS7_STREAM
            if ((ret = wc_PKCS7_AddDataToStream(pkcs7, in, inSz,
                            ASN_TAG_SZ + MAX_LENGTH_SZ, &pkiMsg, &idx)) != 0) {
                return ret;
            }

            pkiMsgSz = wc_PKCS7_GetMaxStream(pkcs7, PKCS7_DEFAULT_PEEK, in, inSz);

            /* restore saved variables */
            expBlockSz = pkcs7->stream->varOne;
#endif
            if (ret == 0 && pkiMsg[idx++] != ASN_OCTET_STRING)
                ret = ASN_PARSE_E;

            if (ret == 0 && GetLength(pkiMsg, &idx, &length, pkiMsgSz) < 0)
                ret = ASN_PARSE_E;

            if (ret == 0 && length != expBlockSz) {
                WOLFSSL_MSG("Incorrect IV length, must be of content alg block size");
                ret = ASN_PARSE_E;
            }

            if (ret != 0) break;
#ifndef NO_PKCS7_STREAM
            /* next chunk of data expected should have the IV */
            pkcs7->stream->expected = length;

            if ((ret = wc_PKCS7_StreamEndCase(pkcs7, &tmpIdx, &idx)) != 0) {
                break;
            }
#endif
            wc_PKCS7_ChangeState(pkcs7, WC_PKCS7_STAGE5);
            FALL_THROUGH;
            /* end of stage 4 */

       case WC_PKCS7_STAGE5:
#ifndef NO_PKCS7_STREAM
            if ((ret = wc_PKCS7_AddDataToStream(pkcs7, in, inSz,
                            pkcs7->stream->expected + ASN_TAG_SZ +
                            MAX_LENGTH_SZ, &pkiMsg, &idx)) != 0) {
                return ret;
            }

            pkiMsgSz = wc_PKCS7_GetMaxStream(pkcs7, PKCS7_DEFAULT_PEEK, in, inSz);

            /* use IV buffer from stream structure */
            tmpIv  = pkcs7->stream->tmpIv;
            length = pkcs7->stream->expected;
#endif
            XMEMCPY(tmpIv, &pkiMsg[idx], length);
            idx += length;
            /* read encryptedContent, cont[0] */
            if (ret == 0 && pkiMsg[idx++] != (ASN_CONTEXT_SPECIFIC | 0))
                ret = ASN_PARSE_E;

            if (ret == 0 && GetLength(pkiMsg, &idx, &encryptedContentSz,
                        pkiMsgSz) <= 0)
                ret = ASN_PARSE_E;

            if (ret < 0)
                break;
#ifndef NO_PKCS7_STREAM
            /* next chunk of data should contain encrypted content */
            pkcs7->stream->varThree = encryptedContentSz;
            if ((ret = wc_PKCS7_StreamEndCase(pkcs7, &tmpIdx, &idx)) != 0) {
                break;
            }

            if (pkcs7->stream->totalRd +  encryptedContentSz < pkiMsgSz) {
                pkcs7->stream->flagOne = 1;
            }

            pkcs7->stream->expected = (pkcs7->stream->maxLen -
                pkcs7->stream->totalRd) + pkcs7->stream->length;

#endif
            wc_PKCS7_ChangeState(pkcs7, WC_PKCS7_STAGE6);
            FALL_THROUGH;
            /* end of stage 5 */

        case WC_PKCS7_STAGE6:
#ifndef NO_PKCS7_STREAM
            //@TODO
            if ((ret = wc_PKCS7_AddDataToStream(pkcs7, in, inSz,
                            pkcs7->stream->expected, &pkiMsg, &idx)) != 0) {
                return ret;
            }

            pkiMsgSz = wc_PKCS7_GetMaxStream(pkcs7, PKCS7_DEFAULT_PEEK, in, inSz);

            /* restore saved variables */
            expBlockSz = pkcs7->stream->varOne;
            encOID     = pkcs7->stream->varTwo;
            encryptedContentSz = pkcs7->stream->varThree;
            version    = pkcs7->stream->vers;
            tmpIv      = pkcs7->stream->tmpIv;
#endif
            if (ret == 0 && (encryptedContent = (byte*)XMALLOC(
                  encryptedContentSz, pkcs7->heap, DYNAMIC_TYPE_PKCS7)) == NULL)
                ret = MEMORY_E;

            if (ret == 0) {
                XMEMCPY(encryptedContent, &pkiMsg[idx], encryptedContentSz);
                idx += encryptedContentSz;
            }

            /* decrypt encryptedContent */
            ret = wc_PKCS7_DecryptContent(encOID, pkcs7->encryptionKey,
                                  pkcs7->encryptionKeySz, tmpIv, expBlockSz,
                                  NULL, 0, NULL, 0, encryptedContent,
                                  encryptedContentSz, encryptedContent);
            if (ret != 0) {
                XFREE(encryptedContent, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
            }

            if (ret == 0) {
                padLen = encryptedContent[encryptedContentSz-1];

                /* copy plaintext to output */
                XMEMCPY(output, encryptedContent, encryptedContentSz - padLen);

                /* get implicit[1] unprotected attributes, optional */
                wc_PKCS7_FreeDecodedAttrib(pkcs7->decodedAttrib, pkcs7->heap);
                pkcs7->decodedAttrib = NULL;
            #ifndef NO_PKCS7_STREAM
                if (pkcs7->stream->flagOne)
            #else
                if (idx < pkiMsgSz)
            #endif
                {
                    haveAttribs = 1;

                    ret = wc_PKCS7_DecodeUnprotectedAttributes(pkcs7, pkiMsg,
                                                       pkiMsgSz, &idx);
                    if (ret != 0) {
                        ForceZero(encryptedContent, encryptedContentSz);
                        XFREE(encryptedContent, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
                        ret = ASN_PARSE_E;
                    }
                }
            }

            if (ret == 0) {
                /* go back and check the version now that attribs have been processed */
                if ((haveAttribs == 0 && version != 0) ||
                    (haveAttribs == 1 && version != 2) ) {
                    XFREE(encryptedContent, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
                    WOLFSSL_MSG("Wrong PKCS#7 EncryptedData version");
                    return ASN_VERSION_E;
                }
                ForceZero(encryptedContent, encryptedContentSz);
                XFREE(encryptedContent, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
                ret = encryptedContentSz - padLen;
            }

            if (ret != 0) break;
        #ifndef NO_PKCS7_STREAM
            wc_PKCS7_ResetStream(pkcs7);
        #endif
            wc_PKCS7_ChangeState(pkcs7, WC_PKCS7_START);
            break;

        default:
            WOLFSSL_MSG("Error in unknown PKCS#7 Decode Encrypted Data state");
            return BAD_STATE_E;
    }

    if (ret != 0) {
    #ifndef NO_PKCS7_STREAM
        /* restart in error case */
        wc_PKCS7_ResetStream(pkcs7);
    #endif
        wc_PKCS7_ChangeState(pkcs7, WC_PKCS7_START);
    }
    return ret;
}

#endif /* NO_PKCS7_ENCRYPTED_DATA */

#if defined(HAVE_LIBZ) && !defined(NO_PKCS7_COMPRESSED_DATA)

/* build PKCS#7 compressedData content type, return encrypted size */
int wc_PKCS7_EncodeCompressedData(PKCS7* pkcs7, byte* output, word32 outputSz)
{
    byte contentInfoSeq[MAX_SEQ_SZ];
    byte contentInfoTypeOid[MAX_OID_SZ];
    byte contentInfoContentSeq[MAX_SEQ_SZ]; /* EXPLICIT [0] */
    byte compressedDataSeq[MAX_SEQ_SZ];
    byte cmsVersion[MAX_VERSION_SZ];
    byte compressAlgId[MAX_ALGO_SZ];
    byte encapContentInfoSeq[MAX_SEQ_SZ];
    byte contentTypeOid[MAX_OID_SZ];
    byte contentSeq[MAX_SEQ_SZ];            /* EXPLICIT [0] */
    byte contentOctetStr[MAX_OCTET_STR_SZ];

    int ret;
    word32 totalSz, idx;
    word32 contentInfoSeqSz, contentInfoContentSeqSz, contentInfoTypeOidSz;
    word32 compressedDataSeqSz, cmsVersionSz, compressAlgIdSz;
    word32 encapContentInfoSeqSz, contentTypeOidSz, contentSeqSz;
    word32 contentOctetStrSz;

    byte* compressed;
    word32 compressedSz;

    if (pkcs7 == NULL || pkcs7->content == NULL || pkcs7->contentSz == 0 ||
        output == NULL || outputSz == 0) {
        return BAD_FUNC_ARG;
    }

    /* allocate space for compressed content. The libz code says the compressed
     * buffer should be srcSz + 0.1% + 12. */
    compressedSz = (pkcs7->contentSz + (word32)(pkcs7->contentSz * 0.001) + 12);
    compressed = (byte*)XMALLOC(compressedSz, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
    if (compressed == NULL) {
        WOLFSSL_MSG("Error allocating memory for CMS compressed content");
        return MEMORY_E;
    }

    /* compress content */
    ret = wc_Compress(compressed, compressedSz, pkcs7->content,
                      pkcs7->contentSz, 0);
    if (ret < 0) {
        XFREE(compressed, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        return ret;
    }
    compressedSz = (word32)ret;

    /* eContent OCTET STRING, working backwards */
    contentOctetStrSz = SetOctetString(compressedSz, contentOctetStr);
    totalSz = contentOctetStrSz + compressedSz;

    /* EXPLICIT [0] eContentType */
    contentSeqSz = SetExplicit(0, totalSz, contentSeq);
    totalSz += contentSeqSz;

    /* eContentType OBJECT IDENTIFIER */
    ret = wc_SetContentType(pkcs7->contentOID, contentTypeOid,
                            sizeof(contentTypeOid));
    if (ret < 0) {
        XFREE(compressed, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        return ret;
    }

    contentTypeOidSz = ret;
    totalSz += contentTypeOidSz;

    /* EncapsulatedContentInfo SEQUENCE */
    encapContentInfoSeqSz = SetSequence(totalSz, encapContentInfoSeq);
    totalSz += encapContentInfoSeqSz;

    /* compressionAlgorithm AlgorithmIdentifier */
    /* Only supports zlib for compression currently:
     * id-alg-zlibCompress (1.2.840.113549.1.9.16.3.8) */
    compressAlgIdSz = SetAlgoID(ZLIBc, compressAlgId, oidCompressType, 0);
    totalSz += compressAlgIdSz;

    /* version */
    cmsVersionSz = SetMyVersion(0, cmsVersion, 0);
    totalSz += cmsVersionSz;

    /* CompressedData SEQUENCE */
    compressedDataSeqSz = SetSequence(totalSz, compressedDataSeq);
    totalSz += compressedDataSeqSz;

    /* ContentInfo content EXPLICIT SEQUENCE */
    contentInfoContentSeqSz = SetExplicit(0, totalSz, contentInfoContentSeq);
    totalSz += contentInfoContentSeqSz;

    /* ContentInfo ContentType (compressedData) */
    ret = wc_SetContentType(COMPRESSED_DATA, contentInfoTypeOid,
                            sizeof(contentInfoTypeOid));
    if (ret < 0) {
        XFREE(compressed, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        return ret;
    }

    contentInfoTypeOidSz = ret;
    totalSz += contentInfoTypeOidSz;

    /* ContentInfo SEQUENCE */
    contentInfoSeqSz = SetSequence(totalSz, contentInfoSeq);
    totalSz += contentInfoSeqSz;

    if (outputSz < totalSz) {
        XFREE(compressed, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        return BUFFER_E;
    }

    idx = 0;
    XMEMCPY(output + idx, contentInfoSeq, contentInfoSeqSz);
    idx += contentInfoSeqSz;
    XMEMCPY(output + idx, contentInfoTypeOid, contentInfoTypeOidSz);
    idx += contentInfoTypeOidSz;
    XMEMCPY(output + idx, contentInfoContentSeq, contentInfoContentSeqSz);
    idx += contentInfoContentSeqSz;
    XMEMCPY(output + idx, compressedDataSeq, compressedDataSeqSz);
    idx += compressedDataSeqSz;
    XMEMCPY(output + idx, cmsVersion, cmsVersionSz);
    idx += cmsVersionSz;
    XMEMCPY(output + idx, compressAlgId, compressAlgIdSz);
    idx += compressAlgIdSz;
    XMEMCPY(output + idx, encapContentInfoSeq, encapContentInfoSeqSz);
    idx += encapContentInfoSeqSz;
    XMEMCPY(output + idx, contentTypeOid, contentTypeOidSz);
    idx += contentTypeOidSz;
    XMEMCPY(output + idx, contentSeq, contentSeqSz);
    idx += contentSeqSz;
    XMEMCPY(output + idx, contentOctetStr, contentOctetStrSz);
    idx += contentOctetStrSz;
    XMEMCPY(output + idx, compressed, compressedSz);
    idx += compressedSz;

    XFREE(compressed, pkcs7->heap, DYNAMIC_TYPE_PKCS7);

    return idx;
}

/* unwrap and decompress PKCS#7/CMS compressedData object,
 * returned decoded size */
int wc_PKCS7_DecodeCompressedData(PKCS7* pkcs7, byte* pkiMsg, word32 pkiMsgSz,
                                  byte* output, word32 outputSz)
{
    int length, version, ret;
    word32 idx = 0, algOID, contentType;

    byte* decompressed;
    word32 decompressedSz;

    if (pkcs7 == NULL || pkiMsg == NULL || pkiMsgSz == 0 ||
        output == NULL || outputSz == 0) {
        return BAD_FUNC_ARG;
    }

    /* get ContentInfo SEQUENCE */
    if (GetSequence(pkiMsg, &idx, &length, pkiMsgSz) < 0)
        return ASN_PARSE_E;

    /* get ContentInfo contentType */
    if (wc_GetContentType(pkiMsg, &idx, &contentType, pkiMsgSz) < 0)
        return ASN_PARSE_E;

    if (contentType != COMPRESSED_DATA)
        return ASN_PARSE_E;

    /* get ContentInfo content EXPLICIT SEQUENCE */
    if (pkiMsg[idx++] != (ASN_CONSTRUCTED | ASN_CONTEXT_SPECIFIC | 0))
        return ASN_PARSE_E;

    if (GetLength(pkiMsg, &idx, &length, pkiMsgSz) < 0)
        return ASN_PARSE_E;

    /* get CompressedData SEQUENCE */
    if (GetSequence(pkiMsg, &idx, &length, pkiMsgSz) < 0)
        return ASN_PARSE_E;

    /* get version */
    if (GetMyVersion(pkiMsg, &idx, &version, pkiMsgSz) < 0)
        return ASN_PARSE_E;

    if (version != 0) {
        WOLFSSL_MSG("CMS CompressedData version MUST be 0, but is not");
        return ASN_PARSE_E;
    }

    /* get CompressionAlgorithmIdentifier */
    if (GetAlgoId(pkiMsg, &idx, &algOID, oidIgnoreType, pkiMsgSz) < 0)
        return ASN_PARSE_E;

    /* Only supports zlib for compression currently:
     * id-alg-zlibCompress (1.2.840.113549.1.9.16.3.8) */
    if (algOID != ZLIBc) {
        WOLFSSL_MSG("CMS CompressedData only supports zlib algorithm");
        return ASN_PARSE_E;
    }

    /* get EncapsulatedContentInfo SEQUENCE */
    if (GetSequence(pkiMsg, &idx, &length, pkiMsgSz) < 0)
        return ASN_PARSE_E;

    /* get ContentType OID */
    if (wc_GetContentType(pkiMsg, &idx, &contentType, pkiMsgSz) < 0)
        return ASN_PARSE_E;

    pkcs7->contentOID = contentType;

    /* get eContent EXPLICIT SEQUENCE */
    if (pkiMsg[idx++] != (ASN_CONSTRUCTED | ASN_CONTEXT_SPECIFIC | 0))
        return ASN_PARSE_E;

    if (GetLength(pkiMsg, &idx, &length, pkiMsgSz) < 0)
        return ASN_PARSE_E;

    /* get content OCTET STRING */
    if (pkiMsg[idx++] != ASN_OCTET_STRING)
        return ASN_PARSE_E;

    if (GetLength(pkiMsg, &idx, &length, pkiMsgSz) < 0)
        return ASN_PARSE_E;

    /* allocate space for decompressed data */
    decompressed = (byte*)XMALLOC(length, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
    if (decompressed == NULL) {
        WOLFSSL_MSG("Error allocating memory for CMS decompression buffer");
        return MEMORY_E;
    }

    /* decompress content */
    ret = wc_DeCompress(decompressed, length, &pkiMsg[idx], length);
    if (ret < 0) {
        XFREE(decompressed, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        return ret;
    }
    decompressedSz = (word32)ret;

    /* get content */
    if (outputSz < decompressedSz) {
        WOLFSSL_MSG("CMS output buffer too small to hold decompressed data");
        XFREE(decompressed, pkcs7->heap, DYNAMIC_TYPE_PKCS7);
        return BUFFER_E;
    }

    XMEMCPY(output, decompressed, decompressedSz);
    XFREE(decompressed, pkcs7->heap, DYNAMIC_TYPE_PKCS7);

    return decompressedSz;
}

#endif /* HAVE_LIBZ && !NO_PKCS7_COMPRESSED_DATA */

#else  /* HAVE_PKCS7 */


#ifdef _MSC_VER
    /* 4206 warning for blank file */
    #pragma warning(disable: 4206)
#endif


#endif /* HAVE_PKCS7 */

