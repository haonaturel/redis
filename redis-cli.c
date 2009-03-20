/* Redis CLI (command line interface)
 *
 * Copyright (c) 2006-2009, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include "anet.h"
#include "sds.h"
#include "adlist.h"

#define REDIS_CMD_INLINE 1
#define REDIS_CMD_BULK 2
#define REDIS_CMD_INTREPLY 4
#define REDIS_CMD_RETCODEREPLY 8
#define REDIS_CMD_BULKREPLY 16
#define REDIS_CMD_MULTIBULKREPLY 32
#define REDIS_CMD_SINGLELINEREPLY 64

#define REDIS_NOTUSED(V) ((void) V)

static struct config {
    char *hostip;
    int hostport;
} config;

struct redisCommand {
    char *name;
    int arity;
    int flags;
};

static struct redisCommand cmdTable[] = {
    {"get",2,REDIS_CMD_INLINE|REDIS_CMD_BULKREPLY},
    {"set",3,REDIS_CMD_BULK|REDIS_CMD_RETCODEREPLY},
    {"setnx",3,REDIS_CMD_BULK|REDIS_CMD_INTREPLY},
    {"del",2,REDIS_CMD_INLINE|REDIS_CMD_INTREPLY},
    {"exists",2,REDIS_CMD_INLINE|REDIS_CMD_INTREPLY},
    {"incr",2,REDIS_CMD_INLINE|REDIS_CMD_INTREPLY},
    {"decr",2,REDIS_CMD_INLINE|REDIS_CMD_INTREPLY},
    {"rpush",3,REDIS_CMD_BULK|REDIS_CMD_RETCODEREPLY},
    {"lpush",3,REDIS_CMD_BULK|REDIS_CMD_RETCODEREPLY},
    {"rpop",2,REDIS_CMD_INLINE|REDIS_CMD_BULKREPLY},
    {"lpop",2,REDIS_CMD_INLINE|REDIS_CMD_BULKREPLY},
    {"llen",2,REDIS_CMD_INLINE|REDIS_CMD_INTREPLY},
    {"lindex",3,REDIS_CMD_INLINE|REDIS_CMD_BULKREPLY},
    {"lset",4,REDIS_CMD_BULK|REDIS_CMD_RETCODEREPLY},
    {"lrange",4,REDIS_CMD_INLINE|REDIS_CMD_MULTIBULKREPLY},
    {"ltrim",4,REDIS_CMD_INLINE|REDIS_CMD_RETCODEREPLY},
    {"lrem",4,REDIS_CMD_BULK|REDIS_CMD_INTREPLY},
    {"sadd",3,REDIS_CMD_BULK|REDIS_CMD_INTREPLY},
    {"srem",3,REDIS_CMD_BULK|REDIS_CMD_INTREPLY},
    {"sismember",3,REDIS_CMD_BULK|REDIS_CMD_INTREPLY},
    {"scard",2,REDIS_CMD_INLINE|REDIS_CMD_INTREPLY},
    {"sinter",-2,REDIS_CMD_INLINE|REDIS_CMD_MULTIBULKREPLY},
    {"sinterstore",-3,REDIS_CMD_INLINE|REDIS_CMD_RETCODEREPLY},
    {"smembers",2,REDIS_CMD_INLINE|REDIS_CMD_MULTIBULKREPLY},
    {"incrby",3,REDIS_CMD_INLINE|REDIS_CMD_INTREPLY},
    {"decrby",3,REDIS_CMD_INLINE|REDIS_CMD_INTREPLY},
    {"randomkey",1,REDIS_CMD_INLINE|REDIS_CMD_SINGLELINEREPLY},
    {"select",2,REDIS_CMD_INLINE|REDIS_CMD_RETCODEREPLY},
    {"move",3,REDIS_CMD_INLINE|REDIS_CMD_INTREPLY},
    {"rename",3,REDIS_CMD_INLINE|REDIS_CMD_RETCODEREPLY},
    {"renamenx",3,REDIS_CMD_INLINE|REDIS_CMD_INTREPLY},
    {"keys",2,REDIS_CMD_INLINE|REDIS_CMD_BULKREPLY},
    {"dbsize",1,REDIS_CMD_INLINE|REDIS_CMD_INTREPLY},
    {"ping",1,REDIS_CMD_INLINE|REDIS_CMD_RETCODEREPLY},
    {"echo",2,REDIS_CMD_BULK|REDIS_CMD_BULKREPLY},
    {"save",1,REDIS_CMD_INLINE|REDIS_CMD_RETCODEREPLY},
    {"bgsave",1,REDIS_CMD_INLINE|REDIS_CMD_RETCODEREPLY},
    {"shutdown",1,REDIS_CMD_INLINE|REDIS_CMD_RETCODEREPLY},
    {"lastsave",1,REDIS_CMD_INLINE|REDIS_CMD_INTREPLY},
    {"type",2,REDIS_CMD_INLINE|REDIS_CMD_SINGLELINEREPLY},
    {"flushdb",1,REDIS_CMD_INLINE|REDIS_CMD_RETCODEREPLY},
    {"flushall",1,REDIS_CMD_INLINE|REDIS_CMD_RETCODEREPLY},
    {"sort",-2,REDIS_CMD_INLINE|REDIS_CMD_MULTIBULKREPLY},
    {"version",1,REDIS_CMD_INLINE|REDIS_CMD_SINGLELINEREPLY},
    {NULL,0,0}
};

static struct redisCommand *lookupCommand(char *name) {
    int j = 0;
    while(cmdTable[j].name != NULL) {
        if (!strcasecmp(name,cmdTable[j].name)) return &cmdTable[j];
        j++;
    }
    return NULL;
}

static int cliConnect(void) {
    char err[ANET_ERR_LEN];
    int fd;

    fd = anetTcpConnect(err,config.hostip,config.hostport);
    if (fd == ANET_ERR) {
        fprintf(stderr,"Connect: %s\n",err);
        return -1;
    }
    anetTcpNoDelay(NULL,fd);
    return fd;
}

static sds cliReadLine(int fd) {
    sds line = sdsempty();

    while(1) {
        char c;

        if (read(fd,&c,1) == -1) {
            sdsfree(line);
            return NULL;
        } else if (c == '\n') {
            break;
        } else {
            line = sdscatlen(line,&c,1);
        }
    }
    return sdstrim(line,"\r\n");
}

static int cliReadInlineReply(int fd, int type) {
    sds reply = cliReadLine(fd);

    if (reply == NULL) return 1;
    printf("%s\n", reply);
    if (type == REDIS_CMD_SINGLELINEREPLY) return 0;
    if (type == REDIS_CMD_INTREPLY) return atoi(reply) < 0;
    if (type == REDIS_CMD_RETCODEREPLY) return reply[0] == '-';
    return 0;
}

static int cliReadBulkReply(int fd, int multibulk) {
    sds replylen = cliReadLine(fd);
    char *reply, crlf[2];
    int bulklen, error = 0;

    if (replylen == NULL) return 1;
    if (strcmp(replylen,"nil") == 0) {
        sdsfree(replylen);
        printf("(nil)\n");
        return 0;
    }
    bulklen = atoi(replylen);
    if (multibulk && bulklen == -1) {
        sdsfree(replylen);
        printf("(nil)");
        return 0;
    }
    if (bulklen < 0) {
        bulklen = -bulklen;
        error = 1;
    }
    reply = malloc(bulklen);
    anetRead(fd,reply,bulklen);
    anetRead(fd,crlf,2);
    fwrite(reply,bulklen,1,stdout);
    if (!multibulk && isatty(fileno(stdout)) && reply[bulklen-1] != '\n')
        printf("\n");
    free(reply);
    return error;
}

static int cliReadMultiBulkReply(int fd) {
    sds replylen = cliReadLine(fd);
    int elements, c = 1;

    if (replylen == NULL) return 1;
    if (strcmp(replylen,"nil") == 0) {
        sdsfree(replylen);
        printf("(nil)\n");
        return 0;
    }
    elements = atoi(replylen);
    while(elements--) {
        printf("%d. ", c);
        if (cliReadBulkReply(fd,1)) return 1;
        printf("\n");
        c++;
    }
    return 0;
}

static int cliSendCommand(int argc, char **argv) {
    struct redisCommand *rc = lookupCommand(argv[0]);
    int fd, j, retval = 0;
    sds cmd = sdsempty();

    if (!rc) {
        fprintf(stderr,"Unknown command '%s'\n",argv[0]);
        return 1;
    }

    if ((rc->arity > 0 && argc != rc->arity) ||
        (rc->arity < 0 && argc < rc->arity)) {
            fprintf(stderr,"Wrong number of arguments for '%s'\n",rc->name);
            return 1;
    }
    if ((fd = cliConnect()) == -1) return 1;

    /* Build the command to send */
    for (j = 0; j < argc; j++) {
        if (j != 0) cmd = sdscat(cmd," ");
        if (j == argc-1 && rc->flags & REDIS_CMD_BULK) {
            cmd = sdscatprintf(cmd,"%d",sdslen(argv[j]));
        } else {
            cmd = sdscatlen(cmd,argv[j],sdslen(argv[j]));
        }
    }
    cmd = sdscat(cmd,"\r\n");
    if (rc->flags & REDIS_CMD_BULK) {
        cmd = sdscatlen(cmd,argv[argc-1],sdslen(argv[argc-1]));
        cmd = sdscat(cmd,"\r\n");
    }
    anetWrite(fd,cmd,sdslen(cmd));
    if (rc->flags & REDIS_CMD_INTREPLY) {
        retval = cliReadInlineReply(fd,REDIS_CMD_INTREPLY);
    } else if (rc->flags & REDIS_CMD_RETCODEREPLY) {
        retval = cliReadInlineReply(fd,REDIS_CMD_RETCODEREPLY);
    } else if (rc->flags & REDIS_CMD_SINGLELINEREPLY) {
        retval = cliReadInlineReply(fd,REDIS_CMD_SINGLELINEREPLY);
    } else if (rc->flags & REDIS_CMD_BULKREPLY) {
        retval = cliReadBulkReply(fd,0);
    } else if (rc->flags & REDIS_CMD_MULTIBULKREPLY) {
        retval = cliReadMultiBulkReply(fd);
    }
    if (retval) {
        close(fd);
        return retval;
    }
    close(fd);
    return 0;
}

static int parseOptions(int argc, char **argv) {
    int i;

    for (i = 1; i < argc; i++) {
        int lastarg = i==argc-1;
        
        if (!strcmp(argv[i],"-h") && !lastarg) {
            char *ip = malloc(32);
            if (anetResolve(NULL,argv[i+1],ip) == ANET_ERR) {
                printf("Can't resolve %s\n", argv[i]);
                exit(1);
            }
            config.hostip = ip;
            i++;
        } else if (!strcmp(argv[i],"-p") && !lastarg) {
            config.hostport = atoi(argv[i+1]);
            i++;
        } else {
            break;
        }
    }
    return i;
}

static sds readArgFromStdin(void) {
    char buf[1024];
    sds arg = sdsempty();

    while(1) {
        int nread = read(fileno(stdin),buf,1024);

        if (nread == 0) break;
        else if (nread == -1) {
            perror("Reading from standard input");
            exit(1);
        }
        arg = sdscatlen(arg,buf,nread);
    }
    return arg;
}

int main(int argc, char **argv) {
    int firstarg, j;
    char **argvcopy;

    config.hostip = "127.0.0.1";
    config.hostport = 6379;

    firstarg = parseOptions(argc,argv);
    argc -= firstarg;
    argv += firstarg;
    
    /* Turn the plain C strings into Sds strings */
    argvcopy = malloc(sizeof(char*)*argc+1);
    for(j = 0; j < argc; j++)
        argvcopy[j] = sdsnew(argv[j]);

    /* Read the last argument from stdandard input */
    if (!isatty(fileno(stdin))) {
        sds lastarg = readArgFromStdin();
        argvcopy[argc] = lastarg;
        argc++;
    }

    if (argc < 1) {
        fprintf(stderr, "usage: redis-cli [-h host] [-p port] cmd arg1 arg2 arg3 ... argN\n");
        fprintf(stderr, "usage: echo \"argN\" | redis-cli [-h host] [-p port] cmd arg1 arg2 ... arg(N-1)\n");
        fprintf(stderr, "example: cat /etc/passwd | redis-cli set my_passwd\n");
        fprintf(stderr, "example: redis-cli get my_passwd\n");
        exit(1);
    }

    return cliSendCommand(argc, argvcopy);
}
