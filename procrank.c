/*
 * Copyright (C) 2008 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/types.h>
#include <unistd.h>
#include <signal.h>

#include <pagemap/pagemap.h>
#include "strlcpy.h"

struct proc_info {
    pid_t pid;
    pm_memusage_t usage;
    uint64_t wss;
};

static void usage(char *myname);
static int getprocname(pid_t pid, char *buf, int len);
static int numcmp(uint64_t a, uint64_t b);

#define declare_sort(field) \
    static int sort_by_ ## field (const void *a, const void *b)

declare_sort(vss);
declare_sort(rss);
declare_sort(pss);
declare_sort(uss);
declare_sort(swap);

int (*compfn)(const void *a, const void *b);
static int order;

enum {
    MEMINFO_TOTAL,
    MEMINFO_FREE,
    MEMINFO_BUFFERS,
    MEMINFO_CACHED,
    MEMINFO_SHMEM,
    MEMINFO_SLAB,
    MEMINFO_SWAP_TOTAL,
    MEMINFO_SWAP_FREE,
    MEMINFO_ZRAM_TOTAL,
    MEMINFO_MAPPED,
    MEMINFO_VMALLOC_USED,
    MEMINFO_PAGE_TABLES,
    MEMINFO_KERNEL_STACK,
    MEMINFO_COUNT
};

// added by charles
#include <map.h>
static int PROCRANK = 0;          /* if you want to show original procrank results , set 1 */
static int DEBUG = 0;             /* if you need more message , set 1 */
static int checkWhat = CHECK_USS;
static int checkContinuousCount = DEFAULT_CHECK_CONTINUOUS_COUNT;
static int checkMaxPeakCount = DEFAULT_CHECK_MAX_PEAK_COUNT;
static int checkWaitingSeconds = DEFAULT_CHECK_WAITING_SECONDS;
static int checkIsSave = OFF;
static char *checkSaveFilename;
static int checkIsLeakCommand = OFF;
static char* checkLeakCommand;
static int tokenidx = 0;
static int tokenoffset[256];
static char *delimiter = "{pid}";

void get_mem_info(uint64_t mem[]) {
    char buffer[1024];
    unsigned int numFound = 0;

    int fd = open("/proc/meminfo", O_RDONLY);

    if (fd < 0) {
        printf("Unable to open /proc/meminfo: %s\n", strerror(errno));
        return;
    }

    const int len = read(fd, buffer, sizeof(buffer)-1);
    close(fd);

    if (len < 0) {
        printf("Empty /proc/meminfo");
        return;
    }
    buffer[len] = 0;

    static const char* const tags[] = {
            "MemTotal:",
            "MemFree:",
            "Buffers:",
            "Cached:",
            "Shmem:",
            "Slab:",
            "SwapTotal:",
            "SwapFree:",
            "ZRam:",            /* not read from meminfo but from /sys/block/zram0 */
            "Mapped:",
            "VmallocUsed:",
            "PageTables:",
            "KernelStack:",
            NULL
    };
    static const int tagsLen[] = {
            9,
            8,
            8,
            7,
            6,
            5,
            10,
            9,
            5,
            7,
            12,
            11,
            12,
            0
    };

    char* p = buffer;
    while (*p && (numFound < (sizeof(tagsLen) / sizeof(tagsLen[0])))) {
        int i = 0;
        while (tags[i]) {
            if (strncmp(p, tags[i], tagsLen[i]) == 0) {
                p += tagsLen[i];
                while (*p == ' ') p++;
                char* num = p;
                while (*p >= '0' && *p <= '9') p++;
                if (*p != 0) {
                    *p = 0;
                    p++;
                }
                mem[i] = atoll(num);
                numFound++;
                break;
            }
            i++;
        }
        while (*p && *p != '\n') {
            p++;
        }
        if (*p) p++;
    }
}

static uint64_t get_zram_mem_used() {
#define ZRAM_SYSFS "/sys/block/zram0/"
    FILE *f = fopen(ZRAM_SYSFS "mm_stat", "r");
    if (f) {
        uint64_t mem_used_total = 0;

        int matched = fscanf(f, "%*d %*d %" SCNu64 " %*d %*d %*d %*d", &mem_used_total);
        if (matched != 1)
            fprintf(stderr, "warning: failed to parse " ZRAM_SYSFS "mm_stat\n");

        fclose(f);
        return mem_used_total;
    }

    f = fopen(ZRAM_SYSFS "mem_used_total", "r");
    if (f) {
        uint64_t mem_used_total = 0;

        int matched = fscanf(f, "%" SCNu64, &mem_used_total);
        if (matched != 1)
            fprintf(stderr, "warning: failed to parse " ZRAM_SYSFS "mem_used_total\n");

        fclose(f);
        return mem_used_total;
    }

    return 0;
}

int main(int argc, char *argv[]) {
    pm_kernel_t *ker;
    pm_process_t *proc;
    pid_t *pids;
    pid_t single_pid = 0;
    struct proc_info **procs;
    size_t num_procs;
    uint64_t total_pss;
    uint64_t total_uss;
    uint64_t total_swap;
    uint64_t total_pswap;
    uint64_t total_uswap;
    uint64_t total_zswap;
    char cmdline[256]; // this must be within the range of int
    int error;
    bool has_swap = false, has_zram = false;
    uint64_t required_flags = 0;
    uint64_t flags_mask = 0;
    uint32_t wait_count=1;

    #define WS_OFF   0
    #define WS_ONLY  1
    #define WS_RESET 2
    int ws;

    int arg;
    size_t i, j;

    uint64_t mem[MEMINFO_COUNT] = { };
    pm_proportional_swap_t *p_swap;
    float zram_cr = 0.0;

    signal(SIGPIPE, SIG_IGN);
    compfn = &sort_by_pss;
    order = -1;
    ws = WS_OFF;

    int c;
    while ((c = getopt (argc, argv, "hpudoc:m:w:s:r:")) != -1){
        switch (c)
        {
        case 'h':
            usage(argv[0]);
            exit(0);
            break;
        case 'p':
            checkWhat = CHECK_PSS;
            break;
        case 'u':
            checkWhat = CHECK_USS;
            break;
        case 'd':
            DEBUG = 1;
            break;
        case 'o':
            PROCRANK = 1;
            break;
        case 'c':
            checkContinuousCount = atoi(optarg);
            break;
        case 'm':
            checkMaxPeakCount = atoi(optarg);
            break;
        case 'w':
            checkWaitingSeconds = atoi(optarg);
            break;
        case 's':
            checkIsSave = ON;
            checkSaveFilename = optarg;
            (void) check_set_save_filename(optarg);
            break;
        case 'r':
            checkIsLeakCommand = ON;
            checkLeakCommand = optarg;
            int dlen = strlen(delimiter);
            int len = strlen(checkLeakCommand);
            for(int tokeni=0;tokeni<(len-dlen);tokeni++){
                if (strncmp(delimiter,checkLeakCommand + tokeni , dlen) == 0){  /* equal */
                    tokenoffset[tokenidx] = tokeni;
                    tokenidx++;
                    if(tokenidx >= 256){
                        fprintf(stderr,"too long , wrong command : tokenidx %d\n",tokenidx);
                        printf("too long , wrong command : tokenidx %d\n",tokenidx);
                        exit(EXIT_FAILURE);
                    }
                }
            }
            for(int tokeni=0;tokeni<tokenidx;tokeni++){
                if(DEBUG) printf("%d token offset %d\n",tokeni,tokenoffset[tokeni]);
            }
            /* 00{ dlen=4 len=3 */
            /* {pid} dlen=4 len=4 */
            /* 0{pid} dlen=4 len=5 */
            break;
        default:
            usage(argv[0]);
            exit(EXIT_FAILURE);
            break;
        }
    }

    printf ("checkWhat = %d, checkContinuousCount = %d, checkMaxPeakCount = %d, checkWaitingSeconds = %d\n",
          checkWhat,checkContinuousCount,checkMaxPeakCount,checkWaitingSeconds);


    /*
    for (arg = 1; arg < argc; arg++) {
        if (!strcmp(argv[arg], "-v")) { compfn = &sort_by_vss; continue; }
        if (!strcmp(argv[arg], "-r")) { compfn = &sort_by_rss; continue; }
        if (!strcmp(argv[arg], "-p")) { compfn = &sort_by_pss; continue; }
        if (!strcmp(argv[arg], "-u")) { compfn = &sort_by_uss; continue; }
        if (!strcmp(argv[arg], "-s")) { compfn = &sort_by_swap; continue; }
        if (!strcmp(argv[arg], "-c")) { required_flags = 0; flags_mask = PM_PAGE_SWAPBACKED; continue; }
        if (!strcmp(argv[arg], "-C")) { required_flags = flags_mask = PM_PAGE_SWAPBACKED; continue; }
        if (!strcmp(argv[arg], "-k")) { required_flags = flags_mask = PM_PAGE_KSM; continue; }
        if (!strcmp(argv[arg], "-w")) { ws = WS_ONLY; continue; }
        if (!strcmp(argv[arg], "-W")) { ws = WS_RESET; continue; }
        if (!strcmp(argv[arg], "-R")) { order *= -1; continue; }
        if (!strcmp(argv[arg], "-P")) { single_pid = atoi(argv[arg+1]); arg++; continue; }
        if (!strcmp(argv[arg], "-h")) { usage(argv[0]); exit(0); }
        fprintf(stderr, "Invalid argument \"%s\".\n", argv[arg]);
        usage(argv[0]);
        exit(EXIT_FAILURE);
    }
    */

    while(1){
        get_mem_info(mem);
        p_swap = pm_memusage_pswap_create(mem[MEMINFO_SWAP_TOTAL] * 1024);
    
        error = pm_kernel_create(&ker);
        if (error) {
            fprintf(stderr, "Error creating kernel interface -- "
                            "does this kernel have pagemap?\n");
            exit(EXIT_FAILURE);
        }
    
        if (single_pid > 0) {
            pids = (pid_t *)malloc(sizeof(pid_t));
            pids[0] = single_pid;
            num_procs = 1;
        } else {
            error = pm_kernel_pids(ker, &pids, &num_procs);
            if (error) {
                fprintf(stderr, "Error listing processes.\n");
                exit(EXIT_FAILURE);
            }
        }
    
        procs = calloc(num_procs, sizeof(struct proc_info*));
        if (procs == NULL) {
            fprintf(stderr, "calloc: %s", strerror(errno));
            exit(EXIT_FAILURE);
        }
    
        for (i = 0; i < num_procs; i++) {
            procs[i] = malloc(sizeof(struct proc_info));
            if (procs[i] == NULL) {
                fprintf(stderr, "malloc: %s\n", strerror(errno));
                exit(EXIT_FAILURE);
            }
            procs[i]->pid = pids[i];
            pm_memusage_zero(&procs[i]->usage);
            pm_memusage_pswap_init_handle(&procs[i]->usage, p_swap);
            error = pm_process_create(ker, pids[i], &proc);
            if (error) {
                fprintf(stderr, "warning: could not create process interface for %d\n", pids[i]);
                continue;
            }
    
            switch (ws) {
            case WS_OFF:
                error = pm_process_usage_flags(proc, &procs[i]->usage, flags_mask,
                                               required_flags);
                break;
            case WS_ONLY:
                error = pm_process_workingset(proc, &procs[i]->usage, 0);
                break;
            case WS_RESET:
                error = pm_process_workingset(proc, NULL, 1);
                break;
            }
    
            if (error) {
                fprintf(stderr, "warning: could not read usage for %d\n", pids[i]);
            }
    
            if (ws != WS_RESET && procs[i]->usage.swap) {
                has_swap = true;
            }
    
            pm_process_destroy(proc);
        }
    
        free(pids);
    
        if (ws == WS_RESET) exit(0);
    
        j = 0;
        for (i = 0; i < num_procs; i++) {
            if (procs[i]->usage.vss) {
                procs[j++] = procs[i];
            } else {
                free(procs[i]);
            }
        }
        num_procs = j;
    
        qsort(procs, num_procs, sizeof(procs[0]), compfn);
    
        if (has_swap) {
            uint64_t zram_mem_used = get_zram_mem_used();
            if (zram_mem_used) {
                mem[MEMINFO_ZRAM_TOTAL] = zram_mem_used/1024;
                zram_cr = (float) mem[MEMINFO_ZRAM_TOTAL] /
                        (mem[MEMINFO_SWAP_TOTAL] - mem[MEMINFO_SWAP_FREE]);
                has_zram = true;
            }
        }
    
        if(PROCRANK) printf("%5s  ", "PID");
        if (ws) {
            if(PROCRANK) printf("%12s  %12s  %12s  ", "WRss", "WPss", "WUss");
            if (has_swap) {
                if(PROCRANK) printf("%12s  %12s  %12s  ", "WSwap", "WPSwap", "WUSwap");
                if (has_zram) {
                    if(PROCRANK) printf("%12s  ", "WZSwap");
                }
            }
        } else {
            if(PROCRANK) printf("%12s  %12s  %12s  %12s  ", "Vss", "Rss", "Pss", "Uss");
            if (has_swap) {
                if(PROCRANK) printf("%12s  %12s  %12s  ", "Swap", "PSwap", "USwap");
                if (has_zram) {
                    if(PROCRANK) printf("%12s  ", "ZSwap");
                }
            }
        }
    
        if(PROCRANK) printf("%s\n", "cmdline");
    
        total_pss = 0;
        total_uss = 0;
        total_swap = 0;
        total_pswap = 0;
        total_uswap = 0;
        total_zswap = 0;
    
        for (i = 0; i < num_procs; i++) {
            if (getprocname(procs[i]->pid, cmdline, (int)sizeof(cmdline)) < 0) {
                /*
                 * Something is probably seriously wrong if writing to the stack
                 * failed.
                 */
                free(procs[i]);
                continue;
            }
    
            total_pss += procs[i]->usage.pss;
            total_uss += procs[i]->usage.uss;
            total_swap += procs[i]->usage.swap;
    
            if(PROCRANK) printf("%5d  ", procs[i]->pid);
    
            if (ws) {
                if(PROCRANK) printf("%11zuK  %11zuK  %11zuK  ",
                    procs[i]->usage.rss / 1024,
                    procs[i]->usage.pss / 1024,
                    procs[i]->usage.uss / 1024
                );
            } else {
                if(PROCRANK) printf("%11zuK  %11zuK  %11zuK  %11zuK  ",
                    procs[i]->usage.vss / 1024,
                    procs[i]->usage.rss / 1024,
                    procs[i]->usage.pss / 1024,
                    procs[i]->usage.uss / 1024
                );
            }
    
            if (has_swap) {
                pm_swapusage_t su;
    
                pm_memusage_pswap_get_usage(&procs[i]->usage, &su);
                if(PROCRANK) printf("%11zuK  ", procs[i]->usage.swap / 1024);
                if(PROCRANK) printf("%11zuK  ", su.proportional / 1024);
                if(PROCRANK) printf("%11zuK  ", su.unique / 1024);
                total_pswap += su.proportional;
                total_uswap += su.unique;
                pm_memusage_pswap_free(&procs[i]->usage);
                if (has_zram) {
                    size_t zpswap = su.proportional * zram_cr;
                    if(PROCRANK) printf("%6zuK  ", zpswap / 1024);
                    total_zswap += zpswap;
                }
            }
    
            if(PROCRANK) printf("%s\n", cmdline);

            int rt;
            rt = check_memory_leak(procs[i]->pid,(int)(procs[i]->usage.vss / 1024),(int)(procs[i]->usage.rss / 1024),(int)(procs[i]->usage.pss / 1024),(int)(procs[i]->usage.uss / 1024),cmdline,checkIsSave,checkWhat,checkContinuousCount,checkMaxPeakCount,DEBUG);
            if ((rt > 0) && (checkIsLeakCommand == ON)){
                /* checkLeakCommand : {pid} -> pid */
                if (tokenidx > 0){
                    char buf[BUFSIZ];
                    char *pp = buf;
                    int len = strlen(checkLeakCommand);
                    int dlen = strlen(delimiter);
                    memset(buf,0,len);
                    int tokenstart = 0;
                    for(int tokeni=0;tokeni<len;){
                        if(tokenoffset[tokenstart] == tokeni){
                            sprintf(buf,"%s%d",pp,procs[i]->pid);
                            if(DEBUG) printf("%d + buf:%s\n",tokeni,buf);
                            tokeni += dlen;
                            tokenstart++;
                        }
                        else {
                            sprintf(buf,"%s%c",pp,checkLeakCommand[tokeni]);
                            if(DEBUG) printf("%d - buf:%s\n",tokeni,buf);
                            tokeni++;
                        }
                    }
                    printf("pid %d:shell %s\n",procs[i]->pid,cmdline);
                    printf("run command : %s\n",buf);
                    system(buf);
                } else if (tokenidx == 0){
                    printf("pid %d:shell %s\n",procs[i]->pid,cmdline);
                    printf("run command : %s\n",checkLeakCommand);
                    system(checkLeakCommand);
                }
            }
    
            free(procs[i]);
        }
    
        free(procs);
        pm_memusage_pswap_destroy(p_swap);
    
        /* process a single process, no need to show total message */
        if (single_pid > 0)
            return 0;
    
        /* Print the separator line */
        printf("%5s  ", "");
    
        if (ws) {
            if(PROCRANK) printf("%12s  %12s  %12s  ", "", "------", "------");
        } else {
            if(PROCRANK) printf("%12s  %12s  %12s  %12s  ", "", "", "------", "------");
        }
    
        if (has_swap) {
            if(PROCRANK) printf("%12s  %12s  %12s  ", "------", "------", "------");
            if (has_zram) {
                if(PROCRANK) printf("%12s  ", "------");
            }
        }
    
        if(PROCRANK) printf("%s\n", "------");
    
        /* Print the total line */
        if(PROCRANK) printf("%5s  ", "");
        if (ws) {
            if(PROCRANK) printf("%12s  %11" PRIu64 "K  %11" PRIu64 "K  ",
                "", total_pss / 1024, total_uss / 1024);
        } else {
            if(PROCRANK) printf("%12s  %12s  %11" PRIu64 "K  %11" PRIu64 "K  ",
                "", "", total_pss / 1024, total_uss / 1024);
        }
    
        if (has_swap) {
            if(PROCRANK) printf("%11" PRIu64 "K  ", total_swap / 1024);
            if(PROCRANK) printf("%11" PRIu64 "K  ", total_pswap / 1024);
            if(PROCRANK) printf("%11" PRIu64 "K  ", total_uswap / 1024);
            if (has_zram) {
                if(PROCRANK) printf("%11" PRIu64 "K  ", total_zswap / 1024);
            }
        }
    
        if(PROCRANK) printf("TOTAL\n");
    
        if(PROCRANK) printf("\n");
    
        if (has_swap) {
            if(PROCRANK) printf("ZRAM: %" PRIu64 "K physical used for %" PRIu64 "K in swap "
                    "(%" PRIu64 "K total swap)\n",
                    mem[MEMINFO_ZRAM_TOTAL], (mem[MEMINFO_SWAP_TOTAL] - mem[MEMINFO_SWAP_FREE]),
                    mem[MEMINFO_SWAP_TOTAL]);
        }
        if(PROCRANK) printf(" RAM: %" PRIu64 "K total, %" PRIu64 "K free, %" PRIu64 "K buffers, "
                "%" PRIu64 "K cached, %" PRIu64 "K shmem, %" PRIu64 "K slab\n",
                mem[MEMINFO_TOTAL], mem[MEMINFO_FREE], mem[MEMINFO_BUFFERS],
                mem[MEMINFO_CACHED], mem[MEMINFO_SHMEM], mem[MEMINFO_SLAB]);
        (void) print_memory_leak(checkWhat,checkIsSave,checkSaveFilename,wait_count);
        printf("\n\ncheckContinuousCount %d : checkMaxPeakCount %d\n",checkContinuousCount,checkMaxPeakCount);
        printf("wait %d seconds : wait count %d\n\n",checkWaitingSeconds,wait_count);
        sleep(checkWaitingSeconds);
	wait_count++;
    }

    return 0;
}

static void usage(char *myname) {
    fprintf(stderr, "Usage: %s [ -p | -u | -d | -o | -h | -c # | -m # | -w # | -s filename | -r command ]\n", myname);
    fprintf(stderr, "    -u  : Check by USS.(default)\n");
    fprintf(stderr, "    -p  : Check by PSS.\n");
    fprintf(stderr, "    -d  : debug on (more message)\n");
    fprintf(stderr, "    -o  : show original procrank msg\n");
    fprintf(stderr, "    -c  Count : contiguous count when it increase continually  (+1 peak count). default %d:\n",DEFAULT_CHECK_CONTINUOUS_COUNT);
    fprintf(stderr, "    -m  MaxPeakCount : peaked count (if reached , it is memory leak for pid). default :%d\n",DEFAULT_CHECK_MAX_PEAK_COUNT);
    fprintf(stderr, "    -w  PeriodicalWaitingSeconds : wating seconds periodically. default :%d\n",DEFAULT_CHECK_WAITING_SECONDS);
    fprintf(stderr, "    -s  filename : writing the memory history. it can be run when it has -s option.\n");
    fprintf(stderr, "    -r  command : run this command when it has memory leak.\n");
    fprintf(stderr, "                  {pid} will be replaced by pid of memory leak process.\n");
    fprintf(stderr, "                  ex) -r \"echo {pid}\"\n");
    fprintf(stderr, "    -h  : Display this help.\n");
}

/*
 * Get the process name for a given PID. Inserts the process name into buffer
 * buf of length len. The size of the buffer must be greater than zero to get
 * any useful output.
 *
 * Note that fgets(3) only declares length as an int, so our buffer size is
 * also declared as an int.
 *
 * Returns 0 on success, a positive value on partial success, and -1 on
 * failure. Other interesting values:
 *   1 on failure to create string to examine proc cmdline entry
 *   2 on failure to open proc cmdline entry
 *   3 on failure to read proc cmdline entry
 */
static int getprocname(pid_t pid, char *buf, int len) {
    char *filename;
    FILE *f;
    int rc = 0;
    static const char* unknown_cmdline = "<unknown>";

    if (len <= 0) {
        return -1;
    }

    if (asprintf(&filename, "/proc/%d/cmdline", pid) < 0) {
        rc = 1;
        goto exit;
    }

    // f = fopen(filename, "r");
    // if (f == NULL) {
    //     rc = 2;
    //     goto releasefilename;
    // }

    // if (fgets(buf, len, f) == NULL) {
    //     rc = 3;
    //     goto closefile;
    // }
    memset(buf,0,256);
    int fd = open(filename, O_RDONLY);
    if(!fd) {
        rc = 2;
        goto releasefilename;
    }
    int nbytesread = read(fd, buf, len);
    if(!nbytesread) {
        rc = 3;
        goto closefile;
    }
    int i = 0;
    for(i = 0; i < nbytesread; i++)
    {
        if(buf[i] == '\0') {
            *(buf + i) = 0x20;
        }
    }
 closefile:
//     (void) fclose(f);
    close(fd);
releasefilename:
    free(filename);
exit:
    if (rc != 0) {
        /*
         * The process went away before we could read its process name. Try
         * to give the user "<unknown>" here, but otherwise they get to look
         * at a blank.
         */
        if (strlcpy(buf, unknown_cmdline, (size_t)len) >= (size_t)len) {
            rc = 4;
        }
    }

    return rc;
}

static int numcmp(uint64_t a, uint64_t b) {
    if (a < b) return -1;
    if (a > b) return 1;
    return 0;
}

#define create_sort(field, compfn) \
    static int sort_by_ ## field (const void *a, const void *b) { \
        return order * compfn( \
            (*((struct proc_info**)a))->usage.field, \
            (*((struct proc_info**)b))->usage.field \
        ); \
    }

create_sort(vss, numcmp)
create_sort(rss, numcmp)
create_sort(pss, numcmp)
create_sort(uss, numcmp)
create_sort(swap, numcmp)
