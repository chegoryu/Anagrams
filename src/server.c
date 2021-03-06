#include "websockets.h"
#include <unicode/utf8.h>
#include <unicode/uchar.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include "config.h"
#include "runner.h"
#include "remap.h"

char* main_page;
int main_len;

const char* e404 = "404, there is no such page";
const char* ebad = "Bad request";
const char* eunexpected = "[unexpected error]";
const char* enomem      = "[server used too much memory]";


struct request_info {
    ws_s* ws;
    int num_found;
    const char* with;
    char tmpbuf[CONFIG_REQUEST_MAX];
};

int match_callback(const char* data, int words, int* length, void* info_) {
    struct request_info* info = (struct request_info*)info_;
    
    if (info->num_found++ >= CONFIG_MAX_RESPONCE)
        return 0;
    
    int inptr = 0;
    int outptr = 0;
    UBool err = 0;

    if (info->with != NULL) {
        info->tmpbuf[outptr++] = '[';

        for (const char* ptr = info->with; *ptr != 0; ++ptr)
            info->tmpbuf[outptr++] = *ptr;
        
        info->tmpbuf[outptr++] = ']';
        info->tmpbuf[outptr++] = ' ';
    }
        
    
    for (int w = 0; w != words; ++w) {
        if (outptr == sizeof(info->tmpbuf))
            goto fail;

        if (w != 0)
            U8_APPEND((uint8_t*)info->tmpbuf, outptr, sizeof(info->tmpbuf), ' ', err);

        if (err)
            goto fail;

        for (int i = 0; i != length[w]; ++i, ++inptr) {
            U8_APPEND((uint8_t*)info->tmpbuf, outptr, sizeof(info->tmpbuf), data[inptr] + CONFIG_UNI_START, err);
            if (err)
                goto fail;
        }
    }
    websocket_write(info->ws, info->tmpbuf, outptr, 1);
    return 1;
fail:
    websocket_write(info->ws, (char*)eunexpected, strlen(eunexpected), 1);
    return 0;
}

int string_to_num_simple(char* str) {
    int r = 0;
    char* p;
    
    for (p = str; *p != 0 && p - str <= 6; ++p) {
        if (*p < '0' || '9' < *p) {
            errno = EINVAL;
            return -1;
        }
        r = 10 * r + (*p - '0');
    }

    if (*p == 0)
        return r;
    errno = EINVAL;
    return -1;
}

void ws_open(ws_s* ws) {
    fprintf(stderr, "Opened a new websocket connection (%p)\n", (void*)ws);

    struct request_info r_info;
    r_info.with      = NULL;
    r_info.ws        = ws;
    r_info.num_found = 0;

    char* udata  = websocket_udata(ws);
    int32_t udata_len  = udata == NULL ? -1 : (int)strlen(udata);
    char* request      = udata_len == -1 ? NULL : malloc(udata_len + 1);
    
    if (request == NULL) {
        websocket_write(ws, (char*)enomem, strlen(enomem), 1);
        websocket_close(ws);
    }

    int freq[CONFIG_ALPH_SIZE];
    for (int i = 0; i != CONFIG_ALPH_SIZE; ++i)
        freq[i] = 0;
    
    int wmin = -1;
    int wmax = -1;
    int lmin = -1;
    int lmax = -1;
    
    int dups   = 0;
    int incomp = 0;
    int rs     = 0;
    
    int32_t outptr = 0;

    int word_adjust = 0;
    
    char* strtok_save;
    for (;; udata = NULL) {
        char* tok = strtok_r(udata, "&", &strtok_save);
        if (tok == NULL)
            break;

        char* peq = strchr(tok, '=');
        if (peq != NULL)
            *peq = 0;
            
        if (strcmp(tok, "q") == 0 || strcmp(tok, "with") == 0) {
            if (peq == NULL)
                goto fail;
            ++peq;

            if (strcmp(tok, "with") == 0) {
                if (r_info.with != NULL)
                    goto fail;
                r_info.with = peq;
            }
            
            int32_t inptr = 0;
            int is = 0;
            
            while (peq[inptr] != 0) {
                UChar32 ch;
                U8_NEXT(peq, inptr, -1, ch);
                ch = remap(ch);	
                
                if (ch < 0)
                   goto fail;
                
                if (CONFIG_UNI_START <= ch && ch < CONFIG_UNI_START + CONFIG_ALPH_SIZE)
                    freq[ch - CONFIG_UNI_START] += (tok[0] == 'q' ? 1 : -1), ++is;
                else if (ch != ' ')
                    goto fail;
                else if (tok[0] == 'w') {
                    word_adjust += (is != 0);
                    is = 0;
                }
            }
            
            if (tok[0] == 'w')
                word_adjust += (is != 0);
        } else if (strcmp(tok, "wmin") == 0) {
            if (peq == NULL)
                goto fail;
            ++peq;
            
            errno = 0;
            wmin = string_to_num_simple(peq);
            if (errno != 0)
                goto fail;
        } else if (strcmp(tok, "wmax") == 0) {
            if (peq == NULL)
                goto fail;
            ++peq;
            
            errno = 0;
            wmax = string_to_num_simple(peq);
            if (errno != 0)
                goto fail;
        } else if (strcmp(tok, "lmin") == 0) {
            if (peq == NULL)
                goto fail;
            ++peq;
            
            errno = 0;
            lmin = string_to_num_simple(peq);
            if (errno != 0)
                goto fail;
        } else if (strcmp(tok, "lmax") == 0) {
            if (peq == NULL)
                goto fail;
            ++peq;
            
            errno = 0;
            lmax = string_to_num_simple(peq);
            if (errno != 0)
                goto fail;
        } else if (strcmp(tok, "r") == 0) {
            if (peq == NULL)
                goto fail;
            ++peq;
            
            errno = 0;
            rs    = string_to_num_simple(peq);
            if (errno != 0)
                goto fail;
        } else if (strcmp(tok, "dups") == 0) {
            if (peq != NULL)
                goto fail;
            dups = 1;
        } else if (strcmp(tok, "inc") == 0) {
            if (peq != NULL)
                goto fail;
            incomp = 1;
        } else {
            goto fail;
        }
    }

    if (wmin != -1)
        wmin -= word_adjust;
    else
        wmin = CONFIG_DEFAULT_WORDS_MIN;
    
    if (wmax != -1)
        wmax -= word_adjust;
    else
        wmax = CONFIG_DEFAULT_WORDS_MAX;

    if (lmin == -1)
        lmin = CONFIG_DEFAULT_SYMB_MIN;
    if (lmax == -1)
        lmax = CONFIG_DEFAULT_SYMB_MAX;
    
    if (wmin < CONFIG_LIMIT_WORDS_MIN || wmax > CONFIG_LIMIT_WORDS_MAX || wmin > wmax)
        goto fail;

    if (lmin < CONFIG_LIMIT_SYMB_MIN || lmax > CONFIG_LIMIT_SYMB_MAX || lmin > lmax)
        goto fail;

    for (int i = 0; i != CONFIG_ALPH_SIZE; ++i) {
        if (freq[i] < 0)
            goto fail;
        
        for (int t = 0; t != freq[i]; ++t)
            request[outptr++] = i;
    }
    
    brute(request, outptr, wmin, wmax, lmin, lmax, match_callback, &r_info, incomp, dups, rs, CONFIG_MAX_TIME);

    free(request);
    websocket_close(ws);
    return;
        
fail:;
    free(request);
    websocket_write(ws, (char*)ebad, strlen(ebad), 1);
    websocket_close(ws);
}

void ws_close(ws_s* ws) {
    free(websocket_udata(ws));
    fprintf(stderr, "Closed websocket connection (%p)\n", (void*)ws);
}

void on_request(http_request_s* request) {
    fprintf(stderr, "request\n");
    
    http_response_s* response = http_response_create(request);
    http_response_log_start(response); // logging
    
    // websocket upgrade.
    if (request->upgrade) {
        if (strcmp(request->path, "/ws") == 0) {
            char* udata = malloc(strlen(request->query) + 1);
            if (udata == NULL)
                return;
            
            http_decode_path_unsafe(udata, request->query);
            
            websocket_upgrade(.request = request,
                              .on_open = ws_open, .on_close = ws_close,
                              .timeout = 40, .response = response,
                              .udata = udata);
        }
        return;
    }
    
    // HTTP response
    if (strcmp(request->path, "/") == 0)
        http_response_write_body(response, main_page, main_len);
    else {
        response->status = 404;
        http_response_write_body(response, e404, strlen(e404));
    }
    
    http_response_finish(response);
}

int init() {
    initBrute(CONFIG_DICT_PATH);
    
    FILE* f = fopen("index.html", "r");
    fseek(f, 0L, SEEK_END);

    main_len = ftell(f);
    main_page = malloc(main_len + 1);

    fseek(f, 0L, SEEK_SET);
    int read = fread(main_page, 1, main_len, f);
    fclose(f);

    return read != main_len ? -1 : 0;
}

int main(void) {
    int errno;
    if ((errno = init()) != 0)
        return errno;
  
    http_listen(CONFIG_SERVER_LOCATION, NULL, .on_request = on_request, .log_static = 1);
    facil_run(.threads = CONFIG_THREAD_COUNT);

    delBrute();
    return 0;
}
