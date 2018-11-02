/*
 * BSD LICENSE
 *
 * Copyright(c) 2014-2017 Intel Corporation. All rights reserved.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in
 *     the documentation and/or other materials provided with the
 *     distribution.
 *   * Neither the name of Intel Corporation nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * @brief Platform QoS utility - main module
 *
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>                                      /**< isspace() */
#include <sys/types.h>                                  /**< open() */
#include <sys/stat.h>
#include <fcntl.h>
#include <getopt.h>                                     /**< getopt_long() */
#include <errno.h>

#include "../lib/pqos.h"

#include "main.h"
#include "profiles.h"
#include "monitor.h"
#include "alloc.h"
#include "cap.h"

#include <signal.h>
#include <python3.6m/Python.h>
#include "pyapi.h"

/**
 * Default CDP configuration option - don't enforce on or off
 */
static enum pqos_cdp_config selfn_l3cdp_config = PQOS_REQUIRE_CDP_ANY;

/**
 * Monitoring reset
 */
static int sel_mon_reset = 0;

/**
 * Maintains pointer to selected log file name
 */
static char *sel_log_file = NULL;

/**
 * Maintains pointer to selected config file
 */
static char *sel_config_file = NULL;

/**
 * Maintains pointer to allocation profile from internal DB
 */
static char *sel_allocation_profile = NULL;

/**
 * Maintains verbose mode choice selected in config string
 */
static int sel_verbose_mode = 0;

/**
 * Reset allocation configuration
 */
static int sel_reset_alloc = 0;

/**
 * Enable showing cache allocation settings
 */
static int sel_show_allocation_config = 0;

/**
 * Enable displaying supported RDT capabilities
 */
static int sel_display = 0;

/**
 * Enable displaying supported RDT capabilities in verbose mode
 */
static int sel_display_verbose = 0;

/**
 * CGROUP DIRS
 * add by quxm 2018.6.22
 */
const char *CG_CPUSET_PREFIX = "/sys/fs/cgroup/cpuset/mysql_test/";
const char *CG_CPU_PREFIX = "/sys/fs/cgroup/cpu/mysql_test/";
const char *CG_MEM_PREFIX = "/sys/fs/cgroup/memory/mysql_test/";
const char *CG_CGROUP_PROC_SUFFIX = "cgroup.procs";
const char *CG_CPUSET_CPUS_SUFFIX = "cpuset.cpus";
const char *CG_TASKS_SUFFIX = "tasks";
const char *CG_YARN_ONLINE_CPUSET = "/sys/fs/cgroup/cpuset/hadoop-yarn/docker-online/";
const char *CG_YARN_OFFLINE_CPUSET = "/sys/fs/cgroup/cpuset/hadoop-yarn/lxc-offline/";



//当前节点的cpu core情况
int ALL_CORES[32];
const int CORE_NUMS = 32;
int OFFLINE_LLC_WAYS = 1;
int ONLINE_LLC_WAYS = 1;
int OFFLINE_MBA_PERCENT = 10;
int OFFLINE_MEM = -1;
const int LLC_WAYS = 2047;

//docker container中的服务线程数的偏移
//mysql类型负载，线程数会多22，包括AM的线程等
//这个值可能根据不同的在线任务而变化
const int TASKS_OFFSET = -22;
//中断监视循环
volatile sig_atomic_t stop_loop = 0;
static void my_handler(int sig){ // can be called asynchronously
    stop_loop = 1; // set flag
}

/**
 * Selected library interface
 */
int sel_interface = PQOS_INTER_MSR;

/**
 * @brief Function to check if a value is already contained in a table
 *
 * @param tab table of values to check
 * @param size size of the table
 * @param val value to search for
 *
 * @return If the value is already in the table
 * @retval 1 if value if found
 * @retval 0 if value is not found
 */
static int
isdup(const uint64_t *tab, const unsigned size, const uint64_t val)
{
        unsigned i;

        for (i = 0; i < size; i++)
                if (tab[i] == val)
                        return 1;
        return 0;
}

uint64_t strtouint64(const char *s)
{
        const char *str = s;
        int base = 10;
        uint64_t n = 0;
        char *endptr = NULL;

        ASSERT(s != NULL);

        if (strncasecmp(s, "0x", 2) == 0) {
                base = 16;
                s += 2;
        }

        n = strtoull(s, &endptr, base);

        if (!(*s != '\0' && *endptr == '\0')) {
                printf("Error converting '%s' to unsigned number!\n", str);
                exit(EXIT_FAILURE);
        }

        return n;
}

unsigned strlisttotab(char *s, uint64_t *tab, const unsigned max)
{
		//第一个参数s=0，2，6 或0-2
        unsigned index = 0;
        char *saveptr = NULL;

        if (s == NULL || tab == NULL || max == 0)
                return index;

        for (;;) {
                char *p = NULL;
                char *token = NULL;

                token = strtok_r(s, ",", &saveptr);
                if (token == NULL)
                        break;

                s = NULL;

                /* get rid of leading spaces & skip empty tokens */
                while (isspace(*token))
                        token++;
                if (*token == '\0')
                        continue;

                p = strchr(token, '-');
                if (p != NULL) {
                        /**
                         * range of numbers provided
                         * example: 1-5 or 12-9
                         */
                        uint64_t n, start, end;
                        *p = '\0';
                        start = strtouint64(token);
                        end = strtouint64(p+1);
                        if (start > end) {
                                /**
                                 * no big deal just swap start with end
                                 */
                                n = start;
                                start = end;
                                end = n;
                        }
                        for (n = start; n <= end; n++) {
                                if (!(isdup(tab, index, n))) {
                                        tab[index] = n;
                                        index++;
                                }
                                if (index >= max)
                                        return index;
                        }
                } else {
                        /**
                         * single number provided here
                         * remove duplicates if necessary
                         */
                        uint64_t val = strtouint64(token);

                        if (!(isdup(tab, index, val))) {
                                tab[index] = val;
                                index++;
                        }
                        if (index >= max)
                                return index;
                }
        }

        return index;
}

__attribute__ ((noreturn)) void
parse_error(const char *arg, const char *note)
{
        printf("Error parsing \"%s\" command line argument. %s\n",
               arg ? arg : "<null>",
               note ? note : "");
        exit(EXIT_FAILURE);
}

void selfn_strdup(char **sel, const char *arg)
{
        ASSERT(sel != NULL && arg != NULL);
        if (*sel != NULL) {
                free(*sel);
                *sel = NULL;
        }
        *sel = strdup(arg);
        ASSERT(*sel != NULL);
        if (*sel == NULL) {
                printf("String duplicate error!\n");
                exit(EXIT_FAILURE);
        }
}

/**
 * @brief Function to print warning to users as utility begins
 */
static void
print_warning(void)
{
#ifdef __linux__
        printf("NOTE:  Mixed use of MSR and kernel interfaces "
               "to manage\n       CAT or CMT & MBM may lead to "
               "unexpected behavior.\n");
#endif
}

/**
 * @brief Selects log file
 *
 * @param arg string passed to -l command line option
 */
static void
selfn_log_file(const char *arg)
{
        selfn_strdup(&sel_log_file, arg);
}

/**
 * @brief Selects verbose mode on
 *
 * @param arg not used
 */
static void
selfn_verbose_mode(const char *arg)
{
        UNUSED_ARG(arg);
        sel_verbose_mode = 1;
}

/**
 * @brief Selects super verbose mode on
 *
 * @param arg not used
 */
static void
selfn_super_verbose_mode(const char *arg)
{
        UNUSED_ARG(arg);
        sel_verbose_mode = 2;
}

/**
 * @brief Sets allocation reset flag
 *
 * @param [in] arg optional configuration string
 *             if NULL or zero length  then configuration check is skipped
 */
static void selfn_reset_alloc(const char *arg)
{
        if (arg != NULL && (strlen(arg) > 0)) {
                const struct {
                        const char *name;
                        enum pqos_cdp_config cdp;
                } patterns[] = {
                        {"l3cdp-on",  PQOS_REQUIRE_CDP_ON},
                        {"l3cdp-off", PQOS_REQUIRE_CDP_OFF},
                        {"l3cdp-any", PQOS_REQUIRE_CDP_ANY},
                };
                unsigned i;

                for (i = 0; i < DIM(patterns); i++)
                        if (strcasecmp(arg, patterns[i].name) == 0)
                                break;

                if (i >= DIM(patterns)) {
                        printf("Unrecognized '%s' allocation "
                               "reset option!\n", arg);
                        exit(EXIT_FAILURE);
                }
                selfn_l3cdp_config = patterns[i].cdp;
        }
        sel_reset_alloc = 1;
}

/**
 * @brief Selects showing allocation settings
 *
 * @param arg not used
 */
static void selfn_show_allocation(const char *arg)
{
        UNUSED_ARG(arg);
        sel_show_allocation_config = 1;
}

/**
 * @brief Selects displaying supported capabilities
 *
 * @param arg not used
 */
static void selfn_display(const char *arg)
{
        UNUSED_ARG(arg);
        sel_display = 1;
}

/**
 * @brief Selects displaying supported capabilities in verbose mode
 *
 * @param arg not used
 */
static void selfn_display_verbose(const char *arg)
{
        UNUSED_ARG(arg);
        sel_display_verbose = 1;
}

/**
 * @brief Selects allocation profile from internal DB
 *
 * @param arg string passed to -c command line option
 */
static void
selfn_allocation_select(const char *arg)
{
        selfn_strdup(&sel_allocation_profile, arg);
}

/**
 * @brief Selects library OS interface
 *
 * @param arg not used
 */
static void
selfn_iface_os(const char *arg)
{
        UNUSED_ARG(arg);
        sel_interface = PQOS_INTER_OS;
}

/**
 * @brief Opens configuration file and parses its contents
 *
 * @param fname Name of the file with configuration parameters
 */
static void
parse_config_file(const char *fname)
{
        if (fname == NULL)
                parse_error("-f", "Invalid configuration file name!\n");

        static const struct {
                const char *option;
                void (*fn)(const char *);
        } optab[] = {
                {"show-alloc:",         selfn_show_allocation },   /**< -s */
                {"display:",            selfn_display },           /**< -d */
                {"display-verbose:",    selfn_display_verbose },   /**< -D */
                {"log-file:",           selfn_log_file },          /**< -l */
                {"verbose-mode:",       selfn_verbose_mode },      /**< -v */
                {"super-verbose-mode:", selfn_super_verbose_mode },/**< -V */
                {"alloc-class-set:",    selfn_allocation_class },  /**< -e */
                {"alloc-assoc-set:",    selfn_allocation_assoc },  /**< -a */
                {"alloc-class-select:", selfn_allocation_select }, /**< -c */
                {"monitor-pids:",       selfn_monitor_pids },      /**< -p */
                {"monitor-cores:",      selfn_monitor_cores },     /**< -m */
                {"monitor-time:",       selfn_monitor_time },      /**< -t */
                {"monitor-interval:",   selfn_monitor_interval },  /**< -i */
                {"monitor-file:",       selfn_monitor_file },      /**< -o */
                {"monitor-file-type:",  selfn_monitor_file_type }, /**< -u */
                {"monitor-top-like:",   selfn_monitor_top_like },  /**< -T */
                {"reset-cat:",          selfn_reset_alloc },       /**< -R */
                {"iface-os:",           selfn_iface_os },          /**< -I */
        };
        FILE *fp = NULL;
        char cb[256];

        fp = fopen(fname, "r");
        if (fp == NULL)
                parse_error(fname, "cannot open configuration file!");

        memset(cb, 0, sizeof(cb));

        while (fgets(cb, sizeof(cb)-1, fp) != NULL) {
                int i, j, remain;
                char *cp = NULL;

                for (j = 0; j < (int)sizeof(cb)-1; j++)
                        if (!isspace(cb[j]))
                                break;

                if (j >= (int)(sizeof(cb)-1))
                        continue; /**< blank line */

                if (strlen(cb+j) == 0)
                        continue; /**< blank line */

                if (cb[j] == '#')
                        continue; /**< comment */

                cp = cb+j;
                remain = (int)strlen(cp);

                /**
                 * remove trailing white spaces
                 */
                for (i = (int)strlen(cp)-1; i > 0; i--)
                        if (!isspace(cp[i])) {
                                cp[i+1] = '\0';
                                break;
                        }

                for (i = 0; i < (int)DIM(optab); i++) {
                        int len = (int)strlen(optab[i].option);

                        if (len > remain)
                                continue;

                        if (strncasecmp(cp, optab[i].option, (size_t)len) != 0)
                                continue;

                        while (isspace(cp[len]))
                                len++; /**< skip space characters */

                        optab[i].fn(cp+len);
                        break;
                }

                if (i >= (int)DIM(optab))
                        parse_error(cp,
                                    "Unrecognized configuration file command");
        }

        fclose(fp);
}
//命令行命令名  quxm comment
static const char *m_cmd_name = "pqos";                     /**< command name */
static const char help_printf_short[] =
        "Usage: %s [-h] [--help] [-v] [--verbose] [-V] [--super-verbose]\n"
        "          [-l FILE] [--log-file=FILE] [-I] [--iface-os]\n"
        "       %s [-s] [--show]\n"
        "       %s [-d] [--display] [-D] [--display-verbose]\n"
        "       %s [-m EVTCORES] [--mon-core=EVTCORES] | [-p [EVTPIDS]] "
        "[--mon-pid[=EVTPIDS]]\n"
        "          [-t SECONDS] [--mon-time=SECONDS]\n"
        "          [-i N] [--mon-interval=N]\n"
        "          [-T] [--mon-top]\n"
        "          [-o FILE] [--mon-file=FILE]\n"
        "          [-u TYPE] [--mon-file-type=TYPE]\n"
        "          [-r] [--mon-reset]\n"
        "       %s [-e CLASSDEF] [--alloc-class=CLASSDEF]\n"
        "          [-a CLASS2ID] [--alloc-assoc=CLASS2ID]\n"
        "       %s [-R] [--alloc-reset]\n"
        "       %s [-H] [--profile-list] | [-c PROFILE] "
        "[--profile-set=PROFILE]\n"
        "       %s [-f FILE] [--config-file=FILE]\n";

static const char help_printf_long[] =
        "Description:\n"
        "  -h, --help                  help page\n"
        "  -v, --verbose               verbose mode\n"
        "  -V, --super-verbose         super-verbose mode\n"
        "  -s, --show                  show current PQoS configuration\n"
        "  -d, --display               display supported capabilities\n"
        "  -D, --display-verbose       display supported capabilities in verbose mode\n"
        "  -f FILE, --config-file=FILE load commands from selected file\n"
        "  -l FILE, --log-file=FILE    log messages into selected file\n"
        "  -e CLASSDEF, --alloc-class=CLASSDEF\n"
        "          define allocation classes.\n"
        "          CLASSDEF format is 'TYPE:ID=DEFINITION;'.\n"
        "          To specify specific resources 'TYPE[@RESOURCE_ID]:ID=DEFINITION;'.\n"
        "          Examples: 'llc:0=0xffff;llc:1=0x00ff;llc@0-1:2=0xff00',\n"
	"                    'llc:0d=0xfff;llc:0c=0xfff00',\n"
        "                    'l2:2=0x3f;l2@2:1=0xf',\n"
        "                    'mba:1=30;mba@1:3=80'.\n"
        "  -a CLASS2ID, --alloc-assoc=CLASS2ID\n"
        "          associate cores/tasks with an allocation class.\n"
        "          CLASS2ID format is 'TYPE:ID=CORE_LIST/TASK_LIST'.\n"
        "          Example 'llc:0=0,2,4,6-10;llc:1=1'.\n"
        "          Example 'core:0=0,2,4,6-10;core:1=1'.\n"
        "          Example 'pid:0=3543,7643,4556;pid:1=7644'.\n"
        "  -R [CONFIG], --alloc-reset[=CONFIG]\n"
        "          reset allocation configuration (L2/L3 CAT & MBA)\n"
        "          CONFIG can be: l3cdp-on, l3cdp-off or l3cdp-any (default).\n"
        "  -m EVTCORES, --mon-core=EVTCORES\n"
        "          select cores and events for monitoring.\n"
        "          EVTCORES format is 'EVENT:CORE_LIST'.\n"
        "          Example: \"all:0,2,4-10;llc:1,3;mbr:11-12\".\n"
        "          Cores can be grouped by enclosing them in square brackets,\n"
        "          example: \"llc:[0-3];all:[4,5,6];mbr:[0-3],7,8\".\n"
        "  -p [EVTPIDS], --mon-pid[=EVTPIDS]\n"
        "          select top 10 most active (CPU utilizing) process ids to monitor\n"
        "          or select process ids and events to monitor.\n"
        "          EVTPIDS format is 'EVENT:PID_LIST'.\n"
        "          Example 'llc:22,25673' or 'all:892,4588-4592'.\n"
        "          Note: processes and cores cannot be monitored together.\n"
        "                Requires Linux and kernel versions 4.1 and newer.\n"
        "  -o FILE, --mon-file=FILE    output monitored data in a FILE\n"
        "  -u TYPE, --mon-file-type=TYPE\n"
        "          select output file format type for monitored data.\n"
        "          TYPE is one of: text (default), xml or csv.\n"
        "  -i N, --mon-interval=N      set sampling interval to Nx100ms,\n"
        "                              default 10 = 10 x 100ms = 1s.\n"
        "  -T, --mon-top               top like monitoring output\n"
        "  -t SECONDS, --mon-time=SECONDS\n"
        "          set monitoring time in seconds. Use 'inf' or 'infinite'\n"
        "          for infinite monitoring. CTRL+C stops monitoring.\n"
        "  -r, --mon-reset             monitoring reset, claim all RMID's\n"
        "  -H, --profile-list          list supported allocation profiles\n"
        "  -c PROFILE, --profile-set=PROFILE\n"
        "          select a PROFILE of predefined allocation classes.\n"
        "          Use -H to list available profiles.\n"
        "  -I, --iface-os\n"
        "          set the library interface to use the kernel\n"
        "          implementation. If not set the default implementation is\n"
        "          to program the MSR's directly.\n";

/**
 * @brief Displays help information
 *
 * @param is_long print long help version or a short one
 *
 */
static void print_help(const int is_long)
{
        printf(help_printf_short,
               m_cmd_name, m_cmd_name, m_cmd_name, m_cmd_name, m_cmd_name,
               m_cmd_name, m_cmd_name, m_cmd_name);
        if (is_long)
                printf(help_printf_long);
}

void init_cores(){
    for(int i=0;i<CORE_NUMS;i++){
        ALL_CORES[i] = 0;
    }
}

static struct option long_cmd_opts[] = {
        {"help",            no_argument,       0, 'h'},
        {"log-file",        required_argument, 0, 'l'},
        {"config-file",     required_argument, 0, 'f'},
        {"show",            no_argument,       0, 's'},
        {"display",         no_argument,       0, 'd'},
        {"display-verbose", no_argument,       0, 'D'},
        {"profile-list",    no_argument,       0, 'H'},
        {"profile-set",     required_argument, 0, 'c'},
        {"mon-interval",    required_argument, 0, 'i'},
        {"mon-pid",         required_argument, 0, 'p'},
        {"mon-core",        required_argument, 0, 'm'},
        {"mon-time",        required_argument, 0, 't'},
        {"mon-top",         no_argument,       0, 'T'},
        {"mon-file",        required_argument, 0, 'o'},
        {"mon-file-type",   required_argument, 0, 'u'},
        {"mon-reset",       no_argument,       0, 'r'},
        {"alloc-class",     required_argument, 0, 'e'},
        {"alloc-reset",     required_argument, 0, 'R'},
        {"alloc-assoc",     required_argument, 0, 'a'},
        {"verbose",         no_argument,       0, 'v'},
        {"super-verbose",   no_argument,       0, 'V'},
        {"iface-os",        no_argument,       0, 'I'},
        {0, 0, 0, 0} /* end */
};

const struct pqos_cpuinfo *p_cpu = NULL;
const struct pqos_cap *p_cap = NULL;
const struct pqos_capability *cap_mon = NULL, *cap_l3ca = NULL,
        *cap_l2ca = NULL, *cap_mba = NULL;

int main(int argc, char **argv)
{
    struct pqos_config cfg;

    unsigned sock_count, *sockets = NULL;
    int cmd, opt,ret, exit_val = EXIT_SUCCESS;
    int opt_index = 0, pid_flag = 0;

    m_cmd_name = argv[0];
    print_warning();

    memset(&cfg, 0, sizeof(cfg));

    //-p参数传入在线任务的pid，将其写入各个cgroup组的cgroup.procs，quxm add 2018.6.23
    char *online_pid = (char*)malloc(20);
    while ((opt = getopt(argc, argv, "p:i")) != -1)
    {
        //printf("opt = %c\n", opt);
        //printf("optarg = %s\n", optarg);
        //printf("optind = %d\n", optind);
        //printf("argv[optind - 1] = %s\n\n",  argv[optind - 1]);
        selfn_verbose_mode(NULL);

        cfg.verbose = sel_verbose_mode;
        cfg.interface = sel_interface;
        /**
         * Set up file descriptor for message log
         */
        if (sel_log_file == NULL) {
                cfg.fd_log = STDOUT_FILENO;
        } else {
                cfg.fd_log = open(sel_log_file, O_WRONLY|O_CREAT,
                                  S_IRUSR|S_IWUSR);
                if (cfg.fd_log == -1) {
                        printf("Error opening %s log file!\n", sel_log_file);
                        exit_val = EXIT_FAILURE;
                        goto error_exit_1;
                }
        }

        ret = pqos_init(&cfg);//输出cache信息
        if (ret != PQOS_RETVAL_OK) {
                printf("Error initializing PQoS library!\n");
                exit_val = EXIT_FAILURE;
                goto error_exit_1;
        }
        //初始化llc等隔离功能
        ret = pqos_cap_get(&p_cap, &p_cpu);
        if (ret != PQOS_RETVAL_OK) {
                printf("Error retrieving PQoS capabilities!\n");
                exit_val = EXIT_FAILURE;
                goto error_exit_2;
        }

        sockets = pqos_cpu_get_sockets(p_cpu, &sock_count);
        if (sockets == NULL) {
                printf("Error retrieving CPU socket information!\n");
                exit_val = EXIT_FAILURE;
                goto error_exit_2;
        }

        ret = pqos_cap_get_type(p_cap, PQOS_CAP_TYPE_MON, &cap_mon);
        if (ret == PQOS_RETVAL_PARAM) {
                printf("Error retrieving monitoring capabilities!\n");
                exit_val = EXIT_FAILURE;
                goto error_exit_2;
        }

        ret = pqos_cap_get_type(p_cap, PQOS_CAP_TYPE_L3CA, &cap_l3ca);
        if (ret == PQOS_RETVAL_PARAM) {
                printf("Error retrieving L3 allocation capabilities!\n");
                exit_val = EXIT_FAILURE;
                goto error_exit_2;
        }

        ret = pqos_cap_get_type(p_cap, PQOS_CAP_TYPE_L2CA, &cap_l2ca);
        if (ret == PQOS_RETVAL_PARAM) {
                printf("Error retrieving L2 allocation capabilities!\n");
                exit_val = EXIT_FAILURE;
                goto error_exit_2;
        }

        ret = pqos_cap_get_type(p_cap, PQOS_CAP_TYPE_MBA, &cap_mba);
        if (ret == PQOS_RETVAL_PARAM) {
                printf("Error retrieving MB allocation capabilities!\n");
                exit_val = EXIT_FAILURE;
                goto error_exit_2;
        }

        switch(opt)
        {
            case 'p':
                //在线profiling组的pid
                //以下操作为将传入的pid参数写入cpu、mem、cpuset子系统下的cgroup.procs文件内
                sprintf(online_pid,"%.*s", strlen(optarg),optarg);
                char *cg_cpu_procs_dir = (char*)malloc(100);
                char *cg_mem_procs_dir = (char*)malloc(100);
                char *cg_cpuset_procs_dir = (char*)malloc(100);
                sprintf(cg_cpu_procs_dir,"%.*s%.*s",strlen(CG_CPU_PREFIX),CG_CPU_PREFIX,
                        strlen(CG_CGROUP_PROC_SUFFIX),CG_CGROUP_PROC_SUFFIX);
                sprintf(cg_mem_procs_dir,"%.*s%.*s",strlen(CG_MEM_PREFIX),CG_MEM_PREFIX,
                        strlen(CG_CGROUP_PROC_SUFFIX),CG_CGROUP_PROC_SUFFIX);
                sprintf(cg_cpuset_procs_dir,"%.*s%.*s",strlen(CG_CPUSET_PREFIX),CG_CPUSET_PREFIX,
                        strlen(CG_CGROUP_PROC_SUFFIX),CG_CGROUP_PROC_SUFFIX);

                FILE *fp_cpu_proc,*fp_cpuset_proc,*fp_mem_proc;
                char *current_cg_procs = (char*)malloc(20);
                fp_cpu_proc = fopen(cg_cpu_procs_dir,"a");
                fp_cpuset_proc = fopen(cg_cpuset_procs_dir,"a");
                fp_mem_proc = fopen(cg_mem_procs_dir,"a");
                if(fp_cpu_proc == NULL || fp_cpuset_proc == NULL || fp_mem_proc == NULL)
                {
                    //error msg: .../cgroup.procs文件不存在
                }
                //虽然pid之前已经被加入其中，并且进程存活，因此无需再次添加
                //经过实验，即使重复添加相同pid也不会影响结果和报错，所以此处先不进行处理
                fprintf(fp_cpu_proc,"%s",online_pid);
                fprintf(fp_cpuset_proc,"%s",online_pid);
                fprintf(fp_mem_proc,"%s",online_pid);
                fclose(fp_cpu_proc);
                fclose(fp_cpuset_proc);
                fclose(fp_mem_proc);
                printf("%s\n","All cgroup.procs write complete!");
                break;
            case 'i':
                //开启后台动态隔离进程
                goto isolation_start;
        }
    }

        //quxm add : dynamic get cores of online cgroup
        //此处的功能为，从cpuset.cpus中读出cores，并赋值给监控项，以便监控在线组的资源使用情况
        char *llc_flag = "llc:1=";
        char *all_flag = "all:[";
        char core_group1[64];
        char *llc_core_groups = (char *)malloc(80);

        //for pqos -a "llc:1={core_group_pqos_a}"
        char *core_group_pqos_a = (char *)malloc(80);

        //目录拼接成"/sys/fs/cgroup/cpuset/在线组mysql_test/cpuset.cpus"
        char *cpuset_cpus_dir = (char *)malloc(100);
        sprintf(cpuset_cpus_dir,"%.*s%.*s",strlen(CG_CPUSET_PREFIX),CG_CPUSET_PREFIX,
                strlen(CG_CPUSET_CPUS_SUFFIX),CG_CPUSET_CPUS_SUFFIX);
        printf("Quxm info:cpuset_cpus_dir = %s\n",cpuset_cpus_dir);
        FILE *fp_cpuset_cpus;
        fp_cpuset_cpus = fopen(cpuset_cpus_dir,"r");
        if(fp_cpuset_cpus == NULL)
        {
                //error message
            printf("ERROR:Can't find the file: cpuset.cpus!");
            return -1;
        }
        fgets(core_group1,sizeof(core_group1),fp_cpuset_cpus);

        int len = strlen(core_group1);

        //del '\n'
        if(core_group1[len-1] == '\n')
            core_group1[len-1] = '\0';


        //core_group_pqos_a = "llc:1=0-31"
        sprintf(core_group_pqos_a,"%.*s%.*s",strlen(llc_flag),llc_flag,
            strlen(core_group1),core_group1);

        printf("Quxm info: Execute : pqos -a = %s\n",core_group_pqos_a);


        sprintf(llc_core_groups,"%.*s%.*s%s",strlen(all_flag),all_flag,
            strlen(core_group1),core_group1,"]");
        printf("Quxm info:Execute : pqos -m = %s\n",llc_core_groups);



        //quxm add: 直接给待监控项赋值，相当于 pqos -m ""
        selfn_monitor_cores(llc_core_groups);

    /*while ((cmd = getopt_long(argc, argv,
                              ":Hhf:i:m:Tt:l:o:u:e:c:a:p:sdDrvVIR:",
                              long_cmd_opts, &opt_index)) != -1) {
            switch (cmd) {
            case 'h':
                    print_help(1);
                    return EXIT_SUCCESS;
            case 'H':
                    profile_l3ca_list(stdout);
                    return EXIT_SUCCESS;
            case 'f':
                    if (sel_config_file != NULL) {
                            printf("Only one config file argument is "
                                   "accepted!\n");
                            return EXIT_FAILURE;
                    }
                    selfn_strdup(&sel_config_file, optarg);
                    parse_config_file(sel_config_file);
                    break;
            case 'i':
                    selfn_monitor_interval(optarg);
                    break;
            case 'p':
                    if (optarg != NULL && *optarg == '-') {
                            *//**
                                 * Next switch option wrongly assumed to be
                                 * argument to '-p'.
                                 * In order to fix it, we are handling this as
                                 * '-p' without parameters (as it should be)
                                 * to start top-pids monitoring mode.
                                 * Have to rewind \a optind as well.
                                 *//*
                                selfn_monitor_top_pids();
                                optind--;
                                break;
                        }
                        selfn_monitor_pids(optarg);
                        pid_flag = 1;
                        break;
                case 'm':
                        selfn_monitor_cores(optarg);
                        break;
                case 't':
                        selfn_monitor_time(optarg);
                        break;
                case 'T':
                        selfn_monitor_top_like(NULL);
                        break;
                case 'l':
                        selfn_log_file(optarg);
                        break;
                case 'o':
                        selfn_monitor_file(optarg);
                        break;
                case 'u':
                        selfn_monitor_file_type(optarg);
                        break;
                case 'e':
                        selfn_allocation_class(optarg);
                        break;
                case 'r':
                        sel_mon_reset = 1;
                        break;
                case 'R':
                        if (optarg != NULL) {
                                if (*optarg == '-') {
                                        *//**
                                        * Next switch option wrongly assumed
                                        * to be argument to '-R'.
                                        * Pass NULL as argument to '-R' function
                                        * and rewind \a optind.
                                        *//*
                                        selfn_reset_alloc(NULL);
                                        optind--;
                                } else
                                        selfn_reset_alloc(optarg);
                        } else
                                selfn_reset_alloc(NULL);
                        break;
                case ':':
                        *//**
                         * This is handler for missing mandatory argument
                         * (enabled by leading ':' in getopt() argument).
                         * -R and -p are only allowed switch for optional args.
                         * Other switches need to report error.
                         *//*
                        if (optopt == 'R') {
                                selfn_reset_alloc(NULL);
                        } else if (optopt == 'p') {
                                *//**
                                 * Top pids mode - in case of '-I -p' top N
                                 * pids (by CPU usage) will be displayed and
                                 * monitored for cache/mbm/misses
                                 *//*
                                selfn_monitor_top_pids();
                                pid_flag = 1;
                        } else {
                                printf("Option -%c is missing required "
                                       "argument\n", optopt);
                                return EXIT_FAILURE;
                        }
                        break;
                case 'a':
                        selfn_allocation_assoc(optarg);
                        pid_flag |= alloc_pid_flag;
                        break;
                case 'c':
                        selfn_allocation_select(optarg);
                        break;
                case 's':
                        selfn_show_allocation(NULL);
                        break;
                case 'd':
                        selfn_display(NULL);
                        break;
                case 'D':
                        selfn_display_verbose(NULL);
                        break;
                case 'v':
                        selfn_verbose_mode(NULL);
                        break;
                case 'V':
                        selfn_super_verbose_mode(NULL);
                        break;
                case 'I':
                        selfn_iface_os(NULL);
                        break;
                default:
                        printf("Unsupported option: -%c. "
                               "See option -h for help.\n", optopt);
                        return EXIT_FAILURE;
                        break;
                case '?':
                        print_help(0);
                        return EXIT_SUCCESS;
                        break;
                }
        }*/

/*        if (pid_flag == 1 && sel_interface == PQOS_INTER_MSR) {
                printf("Error! OS interface option [-I] needed for PID"
                       " operations. Please re-run with the -I option.\n");
                exit_val = EXIT_FAILURE;
                goto error_exit_1;
        }*/
        //selfn_show_allocation(NULL);
        //selfn_allocation_class(optarg);  eg. optarg="llc:1=0x7ff;"
//++++++++++++++++++++++
//        selfn_verbose_mode(NULL);
//        cfg.verbose = sel_verbose_mode;
//        cfg.interface = sel_interface;
//        /**
//         * Set up file descriptor for message log
//         */
//        if (sel_log_file == NULL) {
//                cfg.fd_log = STDOUT_FILENO;
//        } else {
//                cfg.fd_log = open(sel_log_file, O_WRONLY|O_CREAT,
//                                  S_IRUSR|S_IWUSR);
//                if (cfg.fd_log == -1) {
//                        printf("Error opening %s log file!\n", sel_log_file);
//                        exit_val = EXIT_FAILURE;
//                        goto error_exit_1;
//                }
//        }
//
//        ret = pqos_init(&cfg);//输出cache信息
//        if (ret != PQOS_RETVAL_OK) {
//                printf("Error initializing PQoS library!\n");
//                exit_val = EXIT_FAILURE;
//                goto error_exit_1;
//        }
//        //初始化llc等隔离功能
//        ret = pqos_cap_get(&p_cap, &p_cpu);
//        if (ret != PQOS_RETVAL_OK) {
//                printf("Error retrieving PQoS capabilities!\n");
//                exit_val = EXIT_FAILURE;
//                goto error_exit_2;
//        }
//
//        sockets = pqos_cpu_get_sockets(p_cpu, &sock_count);
//        if (sockets == NULL) {
//                printf("Error retrieving CPU socket information!\n");
//                exit_val = EXIT_FAILURE;
//                goto error_exit_2;
//        }
//
//        ret = pqos_cap_get_type(p_cap, PQOS_CAP_TYPE_MON, &cap_mon);
//        if (ret == PQOS_RETVAL_PARAM) {
//                printf("Error retrieving monitoring capabilities!\n");
//                exit_val = EXIT_FAILURE;
//                goto error_exit_2;
//        }
//
//        ret = pqos_cap_get_type(p_cap, PQOS_CAP_TYPE_L3CA, &cap_l3ca);
//        if (ret == PQOS_RETVAL_PARAM) {
//                printf("Error retrieving L3 allocation capabilities!\n");
//                exit_val = EXIT_FAILURE;
//                goto error_exit_2;
//        }
//
//        ret = pqos_cap_get_type(p_cap, PQOS_CAP_TYPE_L2CA, &cap_l2ca);
//        if (ret == PQOS_RETVAL_PARAM) {
//                printf("Error retrieving L2 allocation capabilities!\n");
//                exit_val = EXIT_FAILURE;
//                goto error_exit_2;
//        }
//
//        ret = pqos_cap_get_type(p_cap, PQOS_CAP_TYPE_MBA, &cap_mba);
//        if (ret == PQOS_RETVAL_PARAM) {
//                printf("Error retrieving MB allocation capabilities!\n");
//                exit_val = EXIT_FAILURE;
//                goto error_exit_2;
//        }
//++++++++++++++++++++++++
        //quxm add: 直接执行pqos -a "llc:1=0-31" 2018.6.21
        //写到此处是因为必须要先检测功能，才能进行allocation，如果写前面会报错
        selfn_allocation_assoc(core_group_pqos_a);
        switch (alloc_apply(cap_l3ca, cap_l2ca, cap_mba, p_cpu)) {
            case 0: /* nothing to apply */
                break;
            case 1: /* new allocation config applied and all is good */
                //goto allocation_exit;
                printf("%s\n","Quxm info Online_llc_group : COS1 setting complete!");
                break;
            case -1: /* something went wrong */
            default:
                exit_val = EXIT_FAILURE;
                goto error_exit_2;
                break;
        }
/*        if (sel_mon_reset && cap_mon != NULL) {
                if (pqos_mon_reset() != PQOS_RETVAL_OK) {
                        exit_val = EXIT_FAILURE;
                        printf("CMT/MBM reset failed!\n");
                } else {
                        printf("CMT/MBM reset successful\n");
                }
        }

        if (sel_reset_alloc) {
                *//**
                 * Reset allocation configuration to after-reset state and exit
                 *//*
                if (pqos_alloc_reset(selfn_l3cdp_config) != PQOS_RETVAL_OK) {
                        exit_val = EXIT_FAILURE;
                        printf("Allocation reset failed!\n");
                } else
                        printf("Allocation reset successful\n");
        }

        if (sel_show_allocation_config) {
                *//**
                 * Show info about allocation config and exit
                 *//*
		alloc_print_config(cap_mon, cap_l3ca, cap_l2ca, cap_mba,
                                   sock_count, sockets, p_cpu,
                                   sel_verbose_mode);
                goto allocation_exit;
        }

        if (sel_display || sel_display_verbose) {
                *//**
                 * Display info about supported capabilities
                 *//*
                cap_print_features(p_cap, p_cpu, sel_display_verbose);
                goto allocation_exit;
        }

        if (sel_allocation_profile != NULL) {
                if (profile_l3ca_apply(sel_allocation_profile, cap_l3ca) != 0) {
                        exit_val = EXIT_FAILURE;
                        goto error_exit_2;
                }
        }

        switch (alloc_apply(cap_l3ca, cap_l2ca, cap_mba, p_cpu)) {
        case 0: *//* nothing to apply *//*
                break;
        case 1: *//* new allocation config applied and all is good *//*
                goto allocation_exit;
                break;
        case -1: *//* something went wrong *//*
        default:
                exit_val = EXIT_FAILURE;
                goto error_exit_2;
                break;
        }

        *//**
         * If -R was present ignore all monitoring related options
         *//*
        if (sel_reset_alloc)
                goto allocation_exit;

        *//**
         * Just monitoring option left on the table now
         *//*
        if (cap_mon == NULL) {
                printf("Monitoring capability not detected!\n");
                exit_val = EXIT_FAILURE;
                goto error_exit_2;
        }*/

        if (monitor_setup(p_cpu, cap_mon) != 0) {
                exit_val = EXIT_FAILURE;
                goto error_exit_2;
        }//这里将显示样式设置为text ,sel_output_type = strdup("text");
       //printf("WAYS:%u,cache size:%u KB\n",p_cpu->l3.num_ways,p_cpu->l3.total_size/1024);
        //monitor_loop();
        monitor_loop_quxm();
        monitor_stop();
        goto error_exit_1;

    isolation_start:

        //检测Ctrl_C
        signal(SIGINT, my_handler);
        //初始化cores分配数组
        init_cores();
        int adjust_trigger = 0;
        //初始化python环境
        Py_Initialize();
        if (!Py_IsInitialized()){
            printf("Error : Python Init Failed.\n");
            return -1;
        }
        PyRun_SimpleString("import sys");
        PyRun_SimpleString("sys.path.append('./')");

        //多维资源初始化
        //给python预测脚本传入参数（0，0），即为初始化的参数值，默认得到90%IPS的资源值

        //4维资源的划分在方法内已经执行完毕
        get_online_cores(0,0);

        printf("llcways=%d\n", OFFLINE_LLC_WAYS);
        printf("mbapercents=%d\n", OFFLINE_MBA_PERCENT);

        //提交初始化的配额，使其生效
        isolation_submit();

        //int stop_loop = 0;
        int last_tasks = 0;
        int cur_task_count = 0;
        //循环体外拼接shell command
        char *shell_command_get_tasks = (char *)malloc(300);
        char *get_tasks_shell_dir = "./getTaskCount.sh";
        char shellbuff[100];

        strcpy(shell_command_get_tasks,get_tasks_shell_dir);
        strcat(shell_command_get_tasks," ");
        strcat(shell_command_get_tasks,CG_YARN_ONLINE_CPUSET);
        printf("Get Tasks Shell Command : %s\n",shell_command_get_tasks);


        while(!stop_loop){

            //原采用文件读取方式获取tasks行数，获取子目录文件较为麻烦，已弃用
//            char *docker_tasks_dir = (char *)malloc(200);
//            sprintf(docker_tasks_dir,"%.*s%.*s",strlen(CG_YARN_ONLINE_CPUSET),CG_YARN_ONLINE_CPUSET,
//                    strlen(CG_TASKS_SUFFIX),CG_TASKS_SUFFIX);
//
//            printf("Docker container tasks dir: %s\n",docker_tasks_dir);
//            FILE *fp_docker_tasks;
//
//            cur_task_count = 0;
//            fp_docker_tasks = fopen(docker_tasks_dir,"r");
//            //获取tasks文件行数
//            if(!fp_docker_tasks){
//                printf("Error : FILE %s is null.\n",docker_tasks_dir);
//                return -1;
//            }
//            while(fgets(buf_task,sizeof(buf_task),fp_docker_tasks)){
//                cur_task_count++;
//            }
//
//            free(docker_tasks_dir);
//            fclose(fp_docker_tasks);

            //shell脚本方式，获取所有在线任务的tasks数目

            //1.执行shell获取tasks
            FILE *fshellExecute_tasks=NULL;
            memset(shellbuff,0,sizeof(shellbuff));
            if(NULL==(fshellExecute_tasks=popen(shell_command_get_tasks,"r")))
            {
                fprintf(stderr,"Error : Execute Online command failed: %s",strerror(errno));
            }

            if(NULL!=fgets(shellbuff, sizeof(shellbuff), fshellExecute_tasks)) {
                //printf("%s",shellbuff);
                cur_task_count = atoi(shellbuff);
            }
            pclose(fshellExecute_tasks);

            if(cur_task_count == 0){
                printf("Warning : Docker Container doesn't exit!\n");
            }

            //2.判断tasks值是否波动，如果波动，则触发 动态配额调节
            if(cur_task_count != last_tasks){
                printf("Info : Tasks count changed. Pre is %d, Cur is %d.\n",last_tasks,cur_task_count);

                //两轮tasks数量相差5以上时，再进行调节，5以内的线程波动，默认为可以不进行新的调节
                //保证现有的task+offset的结果是大于10的，否则已开启的线程数太少，无需调节配额
                //task波动过程中，不触发调节，只把trigger置1，当tasks稳定后，再进行调节
                if(abs(cur_task_count - last_tasks) > 5 && (cur_task_count + TASKS_OFFSET) > 10){
                    adjust_trigger = 1;
                }

            }
            else{
                if(adjust_trigger == 0){
                    printf("Info : Tasks count remains unchanged. Cur is %d. Keep monitoring ......\n",cur_task_count);
                }
                else{
                    //触发新一轮的动态调节
                    printf("Info : The load is stable now. Cur is %d. \n",cur_task_count);
                    printf("Info : Dynamic quota adjustment begins ......\n");

                    //调用python预测得到新配额
                    init_cores();

                    get_online_cores(0,cur_task_count + TASKS_OFFSET);
                    //执行调节操作
                    //配额，使其生效
                    isolation_submit();
                    //置0
                    adjust_trigger = 0;
                    printf("Info : Dynamic quota adjustment success.\n");
                }
            }
            last_tasks = cur_task_count;
            if(stop_loop){
                break;
            }
            //sleep 2s
            usleep(2000000);
        }

        Py_Finalize();
        printf("\nMuses Isolation is shutting down.\n");

        return 0;

    allocation_exit:

    error_exit_2:
        ret = pqos_fini();
        ASSERT(ret == PQOS_RETVAL_OK);
        if (ret != PQOS_RETVAL_OK)
                printf("Error shutting down PQoS library!\n");


    error_exit_1:
        monitor_cleanup();

        /**
         * Close file descriptor for message log
         */
        if (cfg.fd_log > 0 && cfg.fd_log != STDOUT_FILENO)
                close(cfg.fd_log);

        /**
         * Free allocated memory
         */
        if (sel_allocation_profile != NULL)
                free(sel_allocation_profile);
        if (sel_log_file != NULL)
                free(sel_log_file);
        if (sel_config_file != NULL)
                free(sel_config_file);
        if (sockets != NULL)
                free(sockets);

        return exit_val;
}

void isolation_submit(){

    //Online and Offline : change cores
    //所有子文件夹都要改才能生效
    //调用update_cpus.sh
    //文件访问的方式已经弃用，采用调用shell脚本对整个子目录的所有cpuset.cpus进行修改
//    char *docker_cpus_dir = (char *)malloc(200);
//    char *lxc_cpus_dir = (char *)malloc(200);
    char *shell_command_online_cpus = (char *)malloc(300);
    char *shell_command_offline_cpus = (char *)malloc(300);
    char *update_cpu_shell_dir = "./update_cpus.sh";


    //文件访问的方式已经弃用，采用调用shell脚本对整个子目录的所有cpuset.cpus进行修改
//    sprintf(docker_cpus_dir,"%.*s%.*s",strlen(CG_YARN_ONLINE_CPUSET),CG_YARN_ONLINE_CPUSET,
//            strlen(CG_CPUSET_CPUS_SUFFIX),CG_CPUSET_CPUS_SUFFIX);
//
//    sprintf(lxc_cpus_dir,"%.*s%.*s",strlen(CG_YARN_OFFLINE_CPUSET),CG_YARN_OFFLINE_CPUSET,
//            strlen(CG_CPUSET_CPUS_SUFFIX),CG_CPUSET_CPUS_SUFFIX);


    //文件访问的方式已经弃用，采用调用shell脚本对整个子目录的所有cpuset.cpus进行修改
//    FILE *fp_docker_cpus,*fp_lxc_cpus;
//    fp_docker_cpus = fopen(docker_cpus_dir,"a");
//    fp_lxc_cpus = fopen(lxc_cpus_dir,"a");

    char *online_cores = (char *)malloc(200);
    char *offline_cores = (char *)malloc(200);
    char *tmp_online_core = (char *)malloc(100);
    char *tmp_offline_core = (char *)malloc(100);
    strcpy(online_cores,"");
    strcpy(offline_cores,"");


    for(int i=0;i<CORE_NUMS;i++){
        if(ALL_CORES[i] == 1){
            sprintf(tmp_online_core,"%d,",i);
            strcat(online_cores,tmp_online_core);
        }
        else{
            sprintf(tmp_offline_core,"%d,",i);
            strcat(offline_cores,tmp_offline_core);
        }
    }
//    printf("Info ：Online cores are %s\n",online_cores);
//    printf("Info ：Offline cores are %s\n",offline_cores);

    //拼接command
    strcpy(shell_command_online_cpus,update_cpu_shell_dir);
    strcat(shell_command_online_cpus," ");
    strcat(shell_command_online_cpus,CG_YARN_ONLINE_CPUSET);
    strcat(shell_command_online_cpus," ");
    strcat(shell_command_online_cpus,online_cores);

    strcpy(shell_command_offline_cpus,update_cpu_shell_dir);
    strcat(shell_command_offline_cpus," ");
    strcat(shell_command_offline_cpus,CG_YARN_OFFLINE_CPUSET);
    strcat(shell_command_offline_cpus," ");
    strcat(shell_command_offline_cpus,offline_cores);

    printf("Online shell command : %s\nOffline shell command : %s\n",shell_command_online_cpus,shell_command_offline_cpus);

    //shell execute
    FILE *fshellExecute_online=NULL;
    char buff[1024];
    memset(buff,0,sizeof(buff));
    if(NULL==(fshellExecute_online=popen(shell_command_online_cpus,"r")))
    {
        fprintf(stderr,"Error : Execute Online command failed: %s",strerror(errno));
    }

    while(NULL!=fgets(buff, sizeof(buff), fshellExecute_online)) {
        printf("%s",buff);
    }
    pclose(fshellExecute_online);

    FILE *fshellExecute_offline=NULL;
    memset(buff,0,sizeof(buff));
    if(NULL==(fshellExecute_offline=popen(shell_command_offline_cpus,"r")))
    {
        fprintf(stderr,"Error : Execute Offline command failed: %s",strerror(errno));
    }

    while(NULL!=fgets(buff, sizeof(buff), fshellExecute_offline)) {
        printf("%s",buff);
    }
    pclose(fshellExecute_offline);



//    fprintf(fp_docker_cpus,"%s",online_cores);
//    fprintf(fp_lxc_cpus,"%s",offline_cores);
//
//
//    fclose(fp_docker_cpus);
//    fclose(fp_lxc_cpus);


    //LLC && MBA

    char *pqos_a_command1 = (char *)malloc(300);
    char *pqos_a_command2 = (char *)malloc(300);
    char *pqos_a_prefix = "./pqos -a \"";
    char *pqos_e_prefix = "./pqos -e \"";
    char *pqos_e_command_llc = (char *)malloc(200);
    char *pqos_e_command_mba = (char *)malloc(200);

    char *llc_flag_cos1 = "llc:1=";
    char *llc_flag_cos2 = "llc:2=";
    char *mba_flag_cos1 = "mba:1=";
    char *mba_flag_cos2 = "mba:2=";

    char *allocation_1 = (char *)malloc(200);
    char *allocation_2 = (char *)malloc(200);

    char *pqos_e_llc2 = (char *)malloc(200);
    char *pqos_e_mba2 = (char *)malloc(200);



    sprintf(allocation_1,"%.*s%.*s",strlen(llc_flag_cos1),llc_flag_cos1,
            strlen(online_cores),online_cores);

    sprintf(allocation_2,"%.*s%.*s",strlen(llc_flag_cos2),llc_flag_cos2,
            strlen(offline_cores),offline_cores);

    //RESET
//    selfn_reset_alloc(NULL);
//    switch (alloc_apply(cap_l3ca, cap_l2ca, cap_mba, p_cpu)) {
//        case 0: /* nothing to apply */
//            printf("%s\n","Quxm info : COS1(online) & COS2(offline) reset return 0!");
//            break;
//        case 1: /* new allocation config applied and all is good */
//            //goto allocation_exit;
//            printf("%s\n","Quxm info : COS1(online) & COS2(offline) reset complete!");
//            break;
//    }

    //submit allocation
    //传参调用方式弃用，换成命令行执行方法（传参在二次执行时会报错，原因暂时未知）

//    selfn_allocation_assoc(allocation_1);
//    switch (alloc_apply(cap_l3ca, cap_l2ca, cap_mba, p_cpu)) {
//        case 0: /* nothing to apply */
//            break;
//        case 1: /* new allocation config applied and all is good */
//            //goto allocation_exit;
//            printf("%s\n","Quxm info Online_llc_group : COS1(online) setting complete!");
//            break;
//    }
    strcpy(pqos_a_command1,pqos_a_prefix);
    strcat(pqos_a_command1,allocation_1);
    strcat(pqos_a_command1,"\"");
    printf("%s\n",pqos_a_command1);

    FILE *fshellExecute_pqosa1=NULL;
    char buffpqos[1024];
    memset(buffpqos,0,sizeof(buffpqos));
    if(NULL==(fshellExecute_pqosa1=popen(pqos_a_command1,"r")))
    {
        fprintf(stderr,"Error : Execute pqos -a command failed: %s",strerror(errno));
    }

    while(NULL!=fgets(buffpqos, sizeof(buffpqos), fshellExecute_pqosa1)) {
        printf("%s",buffpqos);
    }
    pclose(fshellExecute_pqosa1);

//    selfn_allocation_assoc(allocation_2);
//    switch (alloc_apply(cap_l3ca, cap_l2ca, cap_mba, p_cpu)) {
//        case 0: /* nothing to apply */
//            break;
//        case 1: /* new allocation config applied and all is good */
//            //goto allocation_exit;
//            printf("%s\n","Quxm info Offline_llc_group : COS2(offline) setting complete!");
//            break;
//    }

    strcpy(pqos_a_command2,pqos_a_prefix);
    strcat(pqos_a_command2,allocation_2);
    strcat(pqos_a_command2,"\"");
    printf("%s\n",pqos_a_command2);

    FILE *fshellExecute_pqosa2=NULL;
    memset(buffpqos,0,sizeof(buffpqos));
    if(NULL==(fshellExecute_pqosa2=popen(pqos_a_command2,"r")))
    {
        fprintf(stderr,"Error : Execute pqos -a command failed: %s",strerror(errno));
    }

    while(NULL!=fgets(buffpqos, sizeof(buffpqos), fshellExecute_pqosa2)) {
        printf("%s",buffpqos);
    }
    pclose(fshellExecute_pqosa2);


    //change llc & mba
    int current_offline_llc = LLC_WAYS;
    for(int i=0;i<ONLINE_LLC_WAYS;i++){
        current_offline_llc = current_offline_llc >> 1;
    }

    sprintf(pqos_e_llc2,"%.*s%d",strlen(llc_flag_cos2),llc_flag_cos2,current_offline_llc);
    sprintf(pqos_e_mba2,"%.*s%d",strlen(mba_flag_cos2),mba_flag_cos2,OFFLINE_MBA_PERCENT);


    strcpy(pqos_e_command_llc,pqos_e_prefix);
    strcat(pqos_e_command_llc,pqos_e_llc2);
    strcat(pqos_e_command_llc,"\"");

    strcpy(pqos_e_command_mba,pqos_e_prefix);
    strcat(pqos_e_command_mba,pqos_e_mba2);
    strcat(pqos_e_command_mba,"\"");

    printf("%s\n%s\n",pqos_e_command_llc,pqos_e_command_mba);

    FILE *fshellExecute_pqose2llc=NULL;
    memset(buffpqos,0,sizeof(buffpqos));
    if(NULL==(fshellExecute_pqose2llc=popen(pqos_e_command_llc,"r")))
    {
        fprintf(stderr,"Error : Execute pqos -e command failed: %s",strerror(errno));
    }

    while(NULL!=fgets(buffpqos, sizeof(buffpqos), fshellExecute_pqose2llc)) {
        printf("%s",buffpqos);
    }
    pclose(fshellExecute_pqose2llc);

    FILE *fshellExecute_pqose2mba=NULL;
    memset(buffpqos,0,sizeof(buffpqos));
    if(NULL==(fshellExecute_pqose2mba=popen(pqos_e_command_mba,"r")))
    {
        fprintf(stderr,"Error : Execute pqos -e command failed: %s",strerror(errno));
    }

    while(NULL!=fgets(buffpqos, sizeof(buffpqos), fshellExecute_pqose2mba)) {
        printf("%s",buffpqos);
    }
    pclose(fshellExecute_pqose2mba);


    //llc
//    selfn_allocation_class(pqos_e_llc2);
    //memory bandwidth
//    selfn_allocation_class(pqos_e_mba2);

}
/*
int main(int argc, char **argv)
{
        struct pqos_config cfg;
        const struct pqos_cpuinfo *p_cpu = NULL;
        const struct pqos_cap *p_cap = NULL;
        const struct pqos_capability *cap_mon = NULL, *cap_l3ca = NULL,
                *cap_l2ca = NULL, *cap_mba = NULL;
        unsigned sock_count, *sockets = NULL;
        int cmd, ret, exit_val = EXIT_SUCCESS;
        int opt_index = 0, pid_flag = 0;

        m_cmd_name = argv[0];
        print_warning();

        memset(&cfg, 0, sizeof(cfg));

        while ((cmd = getopt_long(argc, argv,
                                  ":Hhf:i:m:Tt:l:o:u:e:c:a:p:sdDrvVIR:",
                                  long_cmd_opts, &opt_index)) != -1) {
                switch (cmd) {
                        case 'h':
                                print_help(1);
                        return EXIT_SUCCESS;
                        case 'H':
                                profile_l3ca_list(stdout);
                        return EXIT_SUCCESS;
                        case 'f':
                                if (sel_config_file != NULL) {
                                        printf("Only one config file argument is "
                                                       "accepted!\n");
                                        return EXIT_FAILURE;
                                }
                        selfn_strdup(&sel_config_file, optarg);
                        parse_config_file(sel_config_file);
                        break;
                        case 'i':
                                selfn_monitor_interval(optarg);
                        break;
                        case 'p':
                                if (optarg != NULL && *optarg == '-') {
                                        *//**
                                         * Next switch option wrongly assumed to be
                                         * argument to '-p'.
                                         * In order to fix it, we are handling this as
                                         * '-p' without parameters (as it should be)
                                         * to start top-pids monitoring mode.
                                         * Have to rewind \a optind as well.
                                         *//*
                                        selfn_monitor_top_pids();
                                        optind--;
                                        break;
                                }
                        selfn_monitor_pids(optarg);
                        pid_flag = 1;
                        break;
                        case 'm':
                                selfn_monitor_cores(optarg);
                        break;
                        case 't':
                                selfn_monitor_time(optarg);
                        break;
                        case 'T':
                                selfn_monitor_top_like(NULL);
                        break;
                        case 'l':
                                selfn_log_file(optarg);
                        break;
                        case 'o':
                                selfn_monitor_file(optarg);
                        break;
                        case 'u':
                                selfn_monitor_file_type(optarg);
                        break;
                        case 'e':
                                selfn_allocation_class(optarg);
                        break;
                        case 'r':
                                sel_mon_reset = 1;
                        break;
                        case 'R':
                                if (optarg != NULL) {
                                        if (*optarg == '-') {
                                                *//**
                                                * Next switch option wrongly assumed
                                                * to be argument to '-R'.
                                                * Pass NULL as argument to '-R' function
                                                * and rewind \a optind.
                                                *//*
                                                selfn_reset_alloc(NULL);
                                                optind--;
                                        } else
                                                selfn_reset_alloc(optarg);
                                } else
                                        selfn_reset_alloc(NULL);
                        break;
                        case ':':
                                *//**
                                 * This is handler for missing mandatory argument
                                 * (enabled by leading ':' in getopt() argument).
                                 * -R and -p are only allowed switch for optional args.
                                 * Other switches need to report error.
                                 *//*
                                if (optopt == 'R') {
                                        selfn_reset_alloc(NULL);
                                } else if (optopt == 'p') {
                                        *//**
                                         * Top pids mode - in case of '-I -p' top N
                                         * pids (by CPU usage) will be displayed and
                                         * monitored for cache/mbm/misses
                                         *//*
                                        selfn_monitor_top_pids();
                                        pid_flag = 1;
                                } else {
                                        printf("Option -%c is missing required "
                                                       "argument\n", optopt);
                                        return EXIT_FAILURE;
                                }
                        break;
                        case 'a':
                                selfn_allocation_assoc(optarg);
                        pid_flag |= alloc_pid_flag;
                        break;
                        case 'c':
                                selfn_allocation_select(optarg);
                        break;
                        case 's':
                                selfn_show_allocation(NULL);
                        break;
                        case 'd':
                                selfn_display(NULL);
                        break;
                        case 'D':
                                selfn_display_verbose(NULL);
                        break;
                        case 'v':
                                selfn_verbose_mode(NULL);
                        break;
                        case 'V':
                                selfn_super_verbose_mode(NULL);
                        break;
                        case 'I':
                                selfn_iface_os(NULL);
                        break;
                        default:
                                printf("Unsupported option: -%c. "
                                               "See option -h for help.\n", optopt);
                        return EXIT_FAILURE;
                        break;
                        case '?':
                                print_help(0);
                        return EXIT_SUCCESS;
                        break;
                }
        }

        if (pid_flag == 1 && sel_interface == PQOS_INTER_MSR) {
                printf("Error! OS interface option [-I] needed for PID"
                               " operations. Please re-run with the -I option.\n");
                exit_val = EXIT_FAILURE;
                goto error_exit_1;
        }
        cfg.verbose = sel_verbose_mode;
        cfg.interface = sel_interface;
        *//**
         * Set up file descriptor for message log
         *//*
        if (sel_log_file == NULL) {
                cfg.fd_log = STDOUT_FILENO;
        } else {
                cfg.fd_log = open(sel_log_file, O_WRONLY|O_CREAT,
                                  S_IRUSR|S_IWUSR);
                if (cfg.fd_log == -1) {
                        printf("Error opening %s log file!\n", sel_log_file);
                        exit_val = EXIT_FAILURE;
                        goto error_exit_1;
                }
        }

        ret = pqos_init(&cfg);
        if (ret != PQOS_RETVAL_OK) {
                printf("Error initializing PQoS library!\n");
                exit_val = EXIT_FAILURE;
                goto error_exit_1;
        }

        ret = pqos_cap_get(&p_cap, &p_cpu);
        if (ret != PQOS_RETVAL_OK) {
                printf("Error retrieving PQoS capabilities!\n");
                exit_val = EXIT_FAILURE;
                goto error_exit_2;
        }

        sockets = pqos_cpu_get_sockets(p_cpu, &sock_count);
        if (sockets == NULL) {
                printf("Error retrieving CPU socket information!\n");
                exit_val = EXIT_FAILURE;
                goto error_exit_2;
        }

        ret = pqos_cap_get_type(p_cap, PQOS_CAP_TYPE_MON, &cap_mon);
        if (ret == PQOS_RETVAL_PARAM) {
                printf("Error retrieving monitoring capabilities!\n");
                exit_val = EXIT_FAILURE;
                goto error_exit_2;
        }

        ret = pqos_cap_get_type(p_cap, PQOS_CAP_TYPE_L3CA, &cap_l3ca);
        if (ret == PQOS_RETVAL_PARAM) {
                printf("Error retrieving L3 allocation capabilities!\n");
                exit_val = EXIT_FAILURE;
                goto error_exit_2;
        }

        ret = pqos_cap_get_type(p_cap, PQOS_CAP_TYPE_L2CA, &cap_l2ca);
        if (ret == PQOS_RETVAL_PARAM) {
                printf("Error retrieving L2 allocation capabilities!\n");
                exit_val = EXIT_FAILURE;
                goto error_exit_2;
        }

        ret = pqos_cap_get_type(p_cap, PQOS_CAP_TYPE_MBA, &cap_mba);
        if (ret == PQOS_RETVAL_PARAM) {
                printf("Error retrieving MB allocation capabilities!\n");
                exit_val = EXIT_FAILURE;
                goto error_exit_2;
        }

        if (sel_mon_reset && cap_mon != NULL) {
                if (pqos_mon_reset() != PQOS_RETVAL_OK) {
                        exit_val = EXIT_FAILURE;
                        printf("CMT/MBM reset failed!\n");
                } else {
                        printf("CMT/MBM reset successful\n");
                }
        }

        if (sel_reset_alloc) {
                *//**
                 * Reset allocation configuration to after-reset state and exit
                 *//*
                if (pqos_alloc_reset(selfn_l3cdp_config) != PQOS_RETVAL_OK) {
                        exit_val = EXIT_FAILURE;
                        printf("Allocation reset failed!\n");
                } else
                        printf("Allocation reset successful\n");
        }

        if (sel_show_allocation_config) {
                *//**
                 * Show info about allocation config and exit
                 *//*
                alloc_print_config(cap_mon, cap_l3ca, cap_l2ca, cap_mba,
                                   sock_count, sockets, p_cpu,
                                   sel_verbose_mode);
                goto allocation_exit;
        }

        if (sel_display || sel_display_verbose) {
                *//**
                 * Display info about supported capabilities
                 *//*
                cap_print_features(p_cap, p_cpu, sel_display_verbose);
                goto allocation_exit;
        }

        if (sel_allocation_profile != NULL) {
                if (profile_l3ca_apply(sel_allocation_profile, cap_l3ca) != 0) {
                        exit_val = EXIT_FAILURE;
                        goto error_exit_2;
                }
        }

        switch (alloc_apply(cap_l3ca, cap_l2ca, cap_mba, p_cpu)) {
                case 0: *//* nothing to apply *//*
                        break;
                case 1: *//* new allocation config applied and all is good *//*
                        goto allocation_exit;
                break;
                case -1: *//* something went wrong *//*
                default:
                        exit_val = EXIT_FAILURE;
                goto error_exit_2;
                break;
        }

        *//**
         * If -R was present ignore all monitoring related options
         *//*
        if (sel_reset_alloc)
                goto allocation_exit;

        *//**
         * Just monitoring option left on the table now
         *//*
        if (cap_mon == NULL) {
                printf("Monitoring capability not detected!\n");
                exit_val = EXIT_FAILURE;
                goto error_exit_2;
        }

        if (monitor_setup(p_cpu, cap_mon) != 0) {
                exit_val = EXIT_FAILURE;
                goto error_exit_2;
        }
        monitor_loop();
        monitor_stop();

    allocation_exit:
    error_exit_2:
        ret = pqos_fini();
        ASSERT(ret == PQOS_RETVAL_OK);
        if (ret != PQOS_RETVAL_OK)
                printf("Error shutting down PQoS library!\n");

    error_exit_1:
        monitor_cleanup();

        *//**
         * Close file descriptor for message log
         *//*
        if (cfg.fd_log > 0 && cfg.fd_log != STDOUT_FILENO)
                close(cfg.fd_log);

        *//**
         * Free allocated memory
         *//*
        if (sel_allocation_profile != NULL)
                free(sel_allocation_profile);
        if (sel_log_file != NULL)
                free(sel_log_file);
        if (sel_config_file != NULL)
                free(sel_config_file);
        if (sockets != NULL)
                free(sockets);

        return exit_val;
}*/
