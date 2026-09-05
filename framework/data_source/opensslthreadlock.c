/***************************************************************************
 *                                  _   _ ____  _
 *  Project                     ___| | | |  _ \| |
 *                             / __| | | | |_) | |
 *                            | (__| |_| |  _ <| |___
 *                             \___|\___/|_| \_\_____|
 *
 * Copyright (C) 1998 - 2017, Daniel Stenberg, <daniel@haxx.se>, et al.
 *
 * This software is licensed as described in the file COPYING, which
 * you should have received as part of this distribution. The terms
 * are also available at https://curl.haxx.se/docs/copyright.html.
 *
 * You may opt to use, copy, modify, merge, publish, distribute and/or sell
 * copies of the Software, and permit persons to whom the Software is
 * furnished to do so, under the terms of the COPYING file.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ***************************************************************************/
/* <DESC>
 * one way to set the necessary OpenSSL locking callbacks if you want to do
 * multi-threaded transfers with HTTPS/FTPS with libcurl built to use OpenSSL.
 * </DESC>
 */
/*
 * OpenSSL >= 1.1.0（含 3.x）内部已是线程安全实现，且 3.0 移除了
 * CRYPTO_set_locking_callback / CRYPTO_num_locks / CRYPTO_set_id_callback
 * 等全局锁回调 API —— 因此这些回调仅对 <1.1.0 的历史版本有意义，
 * 对现代版本本函数为无害空操作。
 *
 * Author: Jeremy Brown
 */

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>

#include <openssl/opensslv.h>

#if OPENSSL_VERSION_NUMBER < 0x10100000L

#include <openssl/err.h>
#include <openssl/crypto.h>

#define MUTEX_TYPE       pthread_mutex_t
#define MUTEX_SETUP(x)   pthread_mutex_init(&(x), NULL)
#define MUTEX_CLEANUP(x) pthread_mutex_destroy(&(x))
#define MUTEX_LOCK(x)    pthread_mutex_lock(&(x))
#define MUTEX_UNLOCK(x)  pthread_mutex_unlock(&(x))
#define THREAD_ID        pthread_self()

/* This array will store all of the mutexes available to OpenSSL. */
static MUTEX_TYPE *mutex_buf = NULL;

static void locking_function(int mode, int n, const char *file, int line)
{
    if (mode & CRYPTO_LOCK)
        MUTEX_LOCK(mutex_buf[n]);
    else
        MUTEX_UNLOCK(mutex_buf[n]);
}

static unsigned long id_function(void)
{
    return ((unsigned long) THREAD_ID);
}

int openssl_thread_setup(void)
{
    int i;

    mutex_buf = malloc(CRYPTO_num_locks() * sizeof(MUTEX_TYPE));
    if (!mutex_buf)
        return 0;
    for (i = 0; i < CRYPTO_num_locks(); i++)
        MUTEX_SETUP(mutex_buf[i]);
    CRYPTO_set_id_callback(id_function);
    CRYPTO_set_locking_callback(locking_function);

    return 1;
}

int openssl_thread_cleanup(void)
{
    int i;

    if (!mutex_buf)
        return 0;
    CRYPTO_set_id_callback(NULL);
    CRYPTO_set_locking_callback(NULL);
    for (i = 0; i < CRYPTO_num_locks(); i++)
        MUTEX_CLEANUP(mutex_buf[i]);
    free(mutex_buf);
    mutex_buf = NULL;
    return 1;
}

#else /* OpenSSL >= 1.1.0 / 3.x：线程安全内建，无需锁回调 */

int openssl_thread_setup(void)
{
    return 1;
}

int openssl_thread_cleanup(void)
{
    return 1;
}

#endif /* OPENSSL_VERSION_NUMBER < 0x10100000L */
