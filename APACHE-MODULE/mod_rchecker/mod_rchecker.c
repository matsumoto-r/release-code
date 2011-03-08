// -------------------------------------------------------------------
// mod_rchecker.c
//   Process Resource Logging Module
//       by rusage().
//   By matsumoto_r Sep 2009 in Japan
//
// Date     2009/12/08
// Version  0.01-beta
//
// change log
// 2009/12/08 matsumoto_r coding start
//
// -------------------------------------------------------------------

// -------------------------------------------------------------------
// How To Compile
// [Use DSO]
// apxs -c -D__MOD_APACHE2__ mod_rchecker.c
// cp ./.libs/mod_rchecker.so /usr/local/apache2/modules
//
// <add to  httpd.conf>
// LoadModule rchecker_module libexec/mod_rchecker.so
//
// -------------------------------------------------------------------

// -------------------------------------------------------------------
// How To Use
// [Server Config]
//
//
// -------------------------------------------------------------------
// [Directive Config]
//
//      log file: /tmp/mod_rchecker.log
//            or #define MOD_RCHECKER_LOG_FILE "/tmp/mod_rchecker.log"
//
// - Logging CPUUserTime
//     RCheckUCPU <threashould> <type>
//
// - Logging CPUSystemTime
//     RCheckSCPU <threashould> <type>
//
// - Logging UsedMemory
//     RCheckMEM <threashould> <type>
// 
//     <threashould>    digit(non-zero)
//
//     <type>           ALL
//                      SELF
//                      CHILD
//
// = Directory Access Control -
// <Directory "/var/www/html">
//      RCheckUCPU 0.0001 ALL
// </Directory>
//
// = File Access Control -
// <Files "ag.cgi">
//      RCheckUCPU 0.003 SELF
//      RCheckSCPU 0.004 CHILD
// </Files>
//
// = Files Regex Access Control -
// <FilesMatch ".*\.cgi$">
//      RCheckUCPU 0.005 ALL
// </FilesMatch>
//
// -------------------------------------------------------------------

/* ---------------------------- */
/* --- Include Header Files --- */
/* ---------------------------- */
#include "httpd.h"
#include "http_config.h"
#include "http_request.h"
#include "http_core.h"
#include "http_protocol.h"
#include "ap_config.h"

#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <libgen.h>
#include <time.h>

#if defined (__MOD_APACHE1__) && defined (__MOD_APACHE2__)
#error Ouch!!
#endif

#ifdef __MOD_APACHE1__
#include "ap_alloc.h"
#endif
#ifdef __MOD_APACHE2__
#include "apr_strings.h"
#endif
#ifdef __MOD_DEBUG__
#include <syslog.h>
#endif


/* ------------------------ */
/* --- Macro Difinition --- */
/* ------------------------ */
#define INITIAL_VALUE              0
#ifdef __MOD_APACHE1__
#define MOD_RCHECKER_LOG_FILE       "/tmp/mod_rchecker.log"
#endif
#ifdef __MOD_APACHE2__
#define MOD_RCHECKERLOG_FILE       "/tmp/mod_rchecker.log"
#define ap_palloc apr_palloc
#define ap_pcalloc apr_pcalloc
#define ap_psprintf apr_psprintf
#define ap_pstrcat apr_pstrcat
#define ap_pstrdup apr_pstrdup
#define ap_pstrndup apr_pstrndup
#define ap_pvsprintf apr_pvsprintf
#define ap_snprintf apr_snprintf
#define ap_vsnprintf apr_vsnprintf
#endif


/* ----------------------------------- */
/* --- Struct and Typed Definition --- */
/* ----------------------------------- */
typedef struct rchecker_dir_conf {

    double cpu_utime;
    double cpu_stime;
    double shared_mem;
    char   *utime_process_type;
    char   *stime_process_type;
    char   *mem_process_type;
    char   *target_dir;

} RCHECKER_D_CONF;

typedef struct rusage_resouce_data {

    double cpu_utime;
    double cpu_stime;
    double shared_mem;

} RESOURCE_DATA;

typedef struct client_access_data {

    char *access_uri;
    char *access_file;
    char *access_src_ip;
    char *access_dst_host;    

} ACCESS_INFO;


/* ----------------------------------- */
/* --- Grobal Variables Definition --- */
/* ----------------------------------- */
char mod_rchecker_version[]           = "mod_version 0.01";
int rchecker_initialized              = 0;
char *mod_rchecker_log_buf            = NULL;
RESOURCE_DATA *pAnalysisResouceBefore = NULL;

#ifdef __MOD_APACHE1__
FILE *mod_rchecker_log_fp = NULL;
#endif
#ifdef __MOD_APACHE2__
apr_file_t *mod_rchecker_log_fp = NULL;
#endif


/* ------------------------- */
/* --- Module Definition --- */
/* ------------------------- */
#ifdef __MOD_APACHE1__
module MODULE_VAR_EXPORT rchecker_module;
#endif
#ifdef __MOD_APACHE2__
module AP_MODULE_DECLARE_DATA rchecker_module;
#endif


/* --------------------------------------- */
/* --- Debug in SYSLOG Logging Routine --- */
/* --------------------------------------- */
#ifdef __MOD_DEBUG__
char *fs_debug_rchecker_log_buf = NULL;
#ifdef __MOD_APACHE1__
void RCHECKER_DEBUG_SYSLOG(const char *key, const char *msg, pool *p)
#endif
#ifdef __MOD_APACHE2__
void RCHECKER_DEBUG_SYSLOG(const char *key, const char *msg, apr_pool_t *p)
#endif
{
    char *fs_buf = NULL;

    fs_buf = (char *)ap_psprintf(p,"%s%s", key, msg);
                                    
    openlog(NULL, LOG_PID, LOG_SYSLOG);
    syslog(LOG_DEBUG, fs_buf);
    closelog();
}
#endif


/* ------------------------------------------- */
/* --- Request Transaction Logging Routine --- */
/* ------------------------------------------- */
#ifdef __MOD_APACHE1__
void _mod_rchecker_logging(double resource_time, double threshold, char *process_type, RCHECKER_D_CONF *pRcheckerDirConf, ACCESS_INFO *pAccessInfoData, const char *msg, pool *p)
#endif
#ifdef __MOD_APACHE2__
void _mod_rchecker_logging(double resource_time, double threshold, char *process_type, RCHECKER_D_CONF *pRcheckerDirConf, ACCESS_INFO *pAccessInfoData, const char *msg, apr_pool_t *p)
#endif
{
    int len;
    time_t t;
    char *log_time;
     
#ifdef __MOD_DEBUG__
    RCHECKER_DEBUG_SYSLOG("_mod_rchecker_logging: ", "start", p);
#endif

    time(&t);
    log_time = (char *)ctime(&t);
    len = strlen(log_time);
    log_time[len - 1] = '\0';
         
    mod_rchecker_log_buf = (char *)ap_psprintf(p
            //, "[%s] pid=%d %s %.5f ] ServerName=(%s) target_dir=(%s) set_cpu_utime=(%.5f) set_cpu_stime=(%.5f) src_ip=(%s) access_file=(%s) access_uri=(%s)\n"
            , "[%s] pid=%d %s %.5f (%s) > threshold=(%.5f) ] config_dir=(%s) src_ip=(%s) access_file=(%s)\n"
            , log_time
            , getpid()
            , msg
            , resource_time
            , process_type
            , threshold
            , pRcheckerDirConf->target_dir
            , pAccessInfoData->access_src_ip
            , pAccessInfoData->access_file
    );
         
#ifdef __MOD_APACHE1__
        fputs(mod_rchecker_log_buf, mod_rchecker_log_fp);
        fflush(mod_la_log_fp);
#endif
#ifdef __MOD_APACHE2__
        apr_file_puts(mod_rchecker_log_buf, mod_rchecker_log_fp);
        apr_file_flush(mod_rchecker_log_fp);
#endif

#ifdef __MOD_DEBUG__
    RCHECKER_DEBUG_SYSLOG("_mod_rchecker_logging: ", "end", p);
#endif

}


/* ------------------------------------------- */
/* --- Init Routine or ap_hook_post_config --- */
/* ------------------------------------------- */
#ifdef __MOD_APACHE1__
static void rchecker_init(server_rec *server, pool *p)
#endif
#ifdef __MOD_APACHE2__
static int rchecker_init(apr_pool_t *p, apr_pool_t *plog, apr_pool_t *ptemp, server_rec *server)
#endif
{
    struct stat;

#ifdef __MOD_DEBUG__
    RCHECKER_DEBUG_SYSLOG("rchecker_init: ", "start", p);
#endif

#ifdef __MOD_APACHE1__
    mod_rchecker_log_fp = (FILE *)ap_pfopen(p, MOD_RCHECKERLOG_FILE, "a");
    if(mod_rchecker_log_fp == NULL){
        return;
    }
#endif
#ifdef __MOD_APACHE2__
    if(apr_file_open(&mod_rchecker_log_fp, MOD_RCHECKERLOG_FILE, APR_WRITE|APR_APPEND|APR_CREATE,
           APR_OS_DEFAULT, p) != APR_SUCCESS){
        return OK;
    }
#endif

    rchecker_initialized = 1;

#ifdef __MOD_DEBUG__
    RCHECKER_DEBUG_SYSLOG("rchecker_init: ", "end", p);
#endif

#ifdef __MOD_APACHE1__
    return;
#endif
#ifdef __MOD_APACHE2__
    return OK;
#endif
}


/* ---------------------------- */
/* --- Create Dir Config --- */
/* ---------------------------- */
#ifdef __MOD_APACHE1__
static void *rchecker_create_dir_config(pool *p, char *d)
#endif
#ifdef __MOD_APACHE2__
static void *rchecker_create_dir_config(apr_pool_t *p, char *dir)
#endif
{
    RCHECKER_D_CONF *pRcheckerDirConf = (RCHECKER_D_CONF *)ap_palloc(p, sizeof(RCHECKER_D_CONF));

#ifdef __MOD_DEBUG__
    RCHECKER_DEBUG_SYSLOG("rchecker_create_dir_config: ", "start", p);
#endif

    pRcheckerDirConf->cpu_utime  = INITIAL_VALUE;
    pRcheckerDirConf->cpu_stime  = INITIAL_VALUE;
    pRcheckerDirConf->shared_mem = INITIAL_VALUE;

    if (dir == NULL) {
        pRcheckerDirConf->target_dir = ap_pstrdup(p, "DocumentRoot");
    } else {
        pRcheckerDirConf->target_dir = ap_pstrdup(p, dir);
    }

#ifdef __MOD_DEBUG__
    RCHECKER_DEBUG_SYSLOG("rchecker_create_dir_config: ", "end", p);
#endif

    return pRcheckerDirConf;
}


/* -------------------------------------------------------------------------------- */
/* --- Set ServerDirective in Struct Command_rec * Cmds (set_cpu_utime_resouce) --- */
/* -------------------------------------------------------------------------------- */
static const char *
set_cpu_utime_resouce(cmd_parms *cmd, void *dir_config_fmt, char *arg1, char *arg2)
{
    RCHECKER_D_CONF *pRcheckerDirConf;

#ifdef __MOD_DEBUG__
    RCHECKER_DEBUG_SYSLOG("set_cpu_utime_resouce: ", "start", cmd->pool);
#endif

    if (strcmp(arg2, "SELF") == -1 && strcmp(arg2, "CHILD") == -1 && strcmp(arg2, "ALL") == -1)
        return "RCheckUCPU: arg2 is SELF or CHILD or ALL!";

    if (atof(arg1) <= 0)
        return "RCheckUCPU: arg1 must be only a number( > 0 )!";

    pRcheckerDirConf = (RCHECKER_D_CONF *)dir_config_fmt;
    pRcheckerDirConf->cpu_utime = atof(arg1);
    pRcheckerDirConf->utime_process_type = ap_pstrdup(cmd->pool, arg2);

#ifdef __MOD_DEBUG__
    fs_debug_rchecker_log_buf = ap_psprintf(cmd->pool
            , "pRcheckerDirConf->target_dir=(%s) pRcheckerDirConf->cpu_utime=(%lf) pRcheckerDirConf->utime_process_type=(%s)"
            , pRcheckerDirConf->target_dir
            , pRcheckerDirConf->cpu_utime
            , pRcheckerDirConf->utime_process_type
    );
    RCHECKER_DEBUG_SYSLOG("set_cpu_utime_resouce: ", fs_debug_rchecker_log_buf, cmd->pool);
#endif

#ifdef __MOD_DEBUG__
    RCHECKER_DEBUG_SYSLOG("set_cpu_utime_resouce: ", "end", cmd->pool);
#endif

    return NULL;
}


/* -------------------------------------------------------------------------------- */
/* --- Set ServerDirective in Struct Command_rec * Cmds (set_cpu_stime_resouce) --- */
/* -------------------------------------------------------------------------------- */
static const char *
set_cpu_stime_resouce(cmd_parms *cmd, void *dir_config_fmt, char *arg1, char *arg2)
{
    RCHECKER_D_CONF *pRcheckerDirConf;

#ifdef __MOD_DEBUG__
    RCHECKER_DEBUG_SYSLOG("set_cpu_stime_resouce: ", "start", cmd->pool);
#endif

    if (strcmp(arg2, "SELF") == -1 && strcmp(arg2, "CHILD") == -1 && strcmp(arg2, "ALL") == -1)
        return "RCheckSCPU: arg2 is SELF or CHILD or ALL!";

    if (atof(arg1) <= 0)
        return "RCheckSCPU: arg1 must be only a number( > 0 )!";

    pRcheckerDirConf = (RCHECKER_D_CONF *)dir_config_fmt;
    pRcheckerDirConf->cpu_stime = atof(arg1);
    pRcheckerDirConf->stime_process_type = ap_pstrdup(cmd->pool, arg2);

#ifdef __MOD_DEBUG__
    fs_debug_rchecker_log_buf = ap_psprintf(cmd->pool
            , "pRcheckerDirConf->target_dir=(%s) pRcheckerDirConf->cpu_stime=(%lf) pRcheckerDirConf->stime_process_type=(%s)"
            , pRcheckerDirConf->target_dir
            , pRcheckerDirConf->cpu_stime
            , pRcheckerDirConf->stime_process_type
    );
    RCHECKER_DEBUG_SYSLOG("set_cpu_stime_resouce: ", fs_debug_rchecker_log_buf, cmd->pool);
#endif

#ifdef __MOD_DEBUG__
    RCHECKER_DEBUG_SYSLOG("set_cpu_stime_resouce: ", "end", cmd->pool);
#endif

    return NULL;
}


/* -------------------------------------------------------------------------------- */
/* --- Set ServerDirective in Struct Command_rec * Cmds (set_shared_mem_resouce) --- */
/* -------------------------------------------------------------------------------- */
static const char *
set_shared_mem_resouce(cmd_parms *cmd, void *dir_config_fmt, char *arg1, char *arg2)
{
    RCHECKER_D_CONF *pRcheckerDirConf;

#ifdef __MOD_DEBUG__
    RCHECKER_DEBUG_SYSLOG("set_shared_mem_resouce: ", "start", cmd->pool);
#endif

    if (strcmp(arg2, "SELF") == -1 && strcmp(arg2, "CHILD") == -1 && strcmp(arg2, "ALL") == -1)
        return "RCheckMEM: arg2 is SELF or CHILD or ALL!";

    if (atof(arg1) <= 0)
        return "RCheckMEM: arg1 must be only a number( > 0 )!";

    pRcheckerDirConf = (RCHECKER_D_CONF *)dir_config_fmt;
    pRcheckerDirConf->shared_mem = atof(arg1);
    pRcheckerDirConf->mem_process_type = ap_pstrdup(cmd->pool, arg2);

#ifdef __MOD_DEBUG__
    fs_debug_rchecker_log_buf = ap_psprintf(cmd->pool
            , "pRcheckerDirConf->target_dir=(%s) pRcheckerDirConf->shared_mem=(%lf) pRcheckerDirConf->mem_process_type=(%s)"
            , pRcheckerDirConf->target_dir
            , pRcheckerDirConf->shared_mem
            , pRcheckerDirConf->mem_process_type
    );
    RCHECKER_DEBUG_SYSLOG("set_shared_mem_resouce: ", fs_debug_rchecker_log_buf, cmd->pool);
#endif

#ifdef __MOD_DEBUG__
    RCHECKER_DEBUG_SYSLOG("set_shared_mem_resouce: ", "end", cmd->pool);
#endif

    return NULL;
}


/* --------------------------- */
/* --- get rutime (doubel) --- */
/* --------------------------- */
static double get_time_from_rutime(time_t sec, suseconds_t usec)
{
    return sec + (double)usec * 1e-6;
}


/* ----------------------------- */
/* --- get process resources --- */
/* ----------------------------- */
static double
#ifdef __MOD_APACHE1__
_get_rusage_utime_resource(pool *p)
#endif
#ifdef __MOD_APACHE2__
_get_rusage_resource(apr_pool_t *p, char *type, char *member)
#endif
{
    struct rusage *resources;
    struct rusage *resources_s;
    struct rusage *resources_c;

    RESOURCE_DATA *pAnalysisResouce;
    pAnalysisResouce = (RESOURCE_DATA *)ap_pcalloc(p, sizeof(RESOURCE_DATA));
    resources = (struct rusage *)ap_pcalloc(p, sizeof(struct rusage));

#ifdef __MOD_DEBUG__
    RCHECKER_DEBUG_SYSLOG("_get_rusage_resource: ", "start", p);
#endif

    if (strcmp(type, "SELF") == 0) {
        if (getrusage(RUSAGE_SELF ,resources) == -1) {
            pAnalysisResouce->cpu_utime = INITIAL_VALUE;
            pAnalysisResouce->cpu_stime = INITIAL_VALUE;
            return -1;
        }
    } else if (strcmp(type, "CHILD") == 0) {
        if (getrusage(RUSAGE_CHILDREN ,resources) == -1) {
            pAnalysisResouce->cpu_utime = INITIAL_VALUE;
            pAnalysisResouce->cpu_stime = INITIAL_VALUE;
            return -1;
        }
    } else if (strcmp(type, "ALL") == 0) {
        resources_s = (struct rusage *)ap_pcalloc(p, sizeof(struct rusage));
        resources_c = (struct rusage *)ap_pcalloc(p, sizeof(struct rusage));
        if (getrusage(RUSAGE_SELF ,resources_s) == -1) {
            pAnalysisResouce->cpu_utime = INITIAL_VALUE;
            pAnalysisResouce->cpu_stime = INITIAL_VALUE;
            return -1;
        }
        if (getrusage(RUSAGE_CHILDREN ,resources_c) == -1) {
            pAnalysisResouce->cpu_utime = INITIAL_VALUE;
            pAnalysisResouce->cpu_stime = INITIAL_VALUE;
            return -1;
        }
        resources->ru_utime.tv_sec  = resources_s->ru_utime.tv_sec + resources_c->ru_utime.tv_sec;
        resources->ru_utime.tv_usec = resources_s->ru_utime.tv_usec + resources_c->ru_utime.tv_usec;
        resources->ru_stime.tv_sec  = resources_s->ru_stime.tv_sec + resources_c->ru_stime.tv_sec;
        resources->ru_stime.tv_usec = resources_s->ru_stime.tv_usec + resources_c->ru_stime.tv_usec;
        resources->ru_minflt        = resources_s->ru_minflt + resources_c->ru_minflt;
    }

    pAnalysisResouce->cpu_utime  = get_time_from_rutime(resources->ru_utime.tv_sec, resources->ru_utime.tv_usec);
    pAnalysisResouce->cpu_stime  = get_time_from_rutime(resources->ru_stime.tv_sec, resources->ru_stime.tv_usec);
    pAnalysisResouce->shared_mem = (((double)resources->ru_minflt * (double)getpagesize() / 1024 / 1024));

#ifdef __MOD_DEBUG__
    fs_debug_rchecker_log_buf = ap_psprintf(p,
            "type=(%s) ru_utime=(%lf) ru_stime=(%lf) ru_utime.tv_sec=(%ld) ru_utime.tv_usec=(%ld) ru_stime.tv_sec=(%ld) ru_stime.tv_usec=(%ld) ru_ixrss=(%ld) ru_idrss=(%ld) ru_isrss=(%ld) ru_minflt=(%ld) ru_majflt=(%ld) ru_nswap=(%ld) ru_inblock=(%ld) ru_oublock=(%ld) ru_msgsnd=(%ld) ru_msgrcv=(%ld) ru_nsignals=(%ld) ru_nvcsw=(%ld) ru_nivcsw=(%ld) getpagesize=(%d)"
            , type
            , pAnalysisResouce->cpu_utime
            , pAnalysisResouce->cpu_stime
            , resources->ru_utime.tv_sec
            , resources->ru_utime.tv_usec
            , resources->ru_stime.tv_sec
            , resources->ru_stime.tv_usec
            , resources->ru_ixrss
            , resources->ru_idrss
            , resources->ru_isrss
            , resources->ru_minflt
            , resources->ru_majflt
            , resources->ru_nswap
            , resources->ru_inblock
            , resources->ru_oublock
            , resources->ru_msgsnd
            , resources->ru_msgrcv
            , resources->ru_nsignals
            , resources->ru_nvcsw
            , resources->ru_nivcsw
            , getpagesize()
    );
    RCHECKER_DEBUG_SYSLOG("_get_rusage_resource: ", fs_debug_rchecker_log_buf, p);
#endif

#ifdef __MOD_DEBUG__
    RCHECKER_DEBUG_SYSLOG("_get_rusage_resource: ", "end", p);
#endif

    if (strcmp(member, "cpu_utime") == 0) {
        return pAnalysisResouce->cpu_utime;
    } else if (strcmp(member, "cpu_stime") == 0) {
        return pAnalysisResouce->cpu_stime;
    } else if (strcmp(member, "shared_mem") == 0) {
        return pAnalysisResouce->shared_mem;
    }
    
    return -1;
}


/* ----------------------------------------------- */
/* --- Access Checker (ap_hook_access_checker) --- */
/* ----------------------------------------------- */
static int before_resource_checker(request_rec *r)
{
    int match;
    struct stat sb;

    pAnalysisResouceBefore = (RESOURCE_DATA *)ap_pcalloc(r->pool, sizeof(RESOURCE_DATA));

    RCHECKER_D_CONF *pRcheckerDirConf;
    pRcheckerDirConf = (RCHECKER_D_CONF *)ap_get_module_config(r->per_dir_config, &rchecker_module);

#ifdef __MOD_DEBUG__
    RCHECKER_DEBUG_SYSLOG("before_resource_checker: ", "start", r->pool);
#endif


    if (rchecker_initialized == 0) { 
        return OK;
    }

#ifdef __MOD_APACHE1__
    if (r->main) {
        return OK;
    }
#endif

#ifdef __MOD_APACHE2__
    if (r->main && (stat(r->filename, &sb) == -1) && errno == ENOENT) {
       return OK;
    }
#endif

#ifdef __MOD_DEBUG__
    fs_debug_rchecker_log_buf = ap_psprintf(r->pool,
            "pRcheckerDirConf: pRcheckerDirConf->target_dir=(%s) pRcheckerDirConf->cpu_utime=(%lf) pRcheckerDirConf->cpu_stime=(%lf) pRcheckerDirConf->shared_mem=(%lf)"
            , pRcheckerDirConf->target_dir
            , pRcheckerDirConf->cpu_utime
            , pRcheckerDirConf->cpu_stime
            , pRcheckerDirConf->shared_mem
    );
    RCHECKER_DEBUG_SYSLOG("before_resource_checker: ", fs_debug_rchecker_log_buf, r->pool);
#endif

    match = 0;
    pAnalysisResouceBefore->cpu_utime  = INITIAL_VALUE;
    pAnalysisResouceBefore->cpu_stime  = INITIAL_VALUE;
    pAnalysisResouceBefore->shared_mem = INITIAL_VALUE;

    if (pRcheckerDirConf->cpu_utime > INITIAL_VALUE) {
        match = 1;
        pAnalysisResouceBefore->cpu_utime  = _get_rusage_resource(r->pool, pRcheckerDirConf->utime_process_type, "cpu_utime");
    }

    if (pRcheckerDirConf->cpu_stime > INITIAL_VALUE) {
        match = 1;
        pAnalysisResouceBefore->cpu_stime  = _get_rusage_resource(r->pool, pRcheckerDirConf->stime_process_type, "cpu_stime");
    }

    if (pRcheckerDirConf->shared_mem > INITIAL_VALUE) {
        match = 1;
        pAnalysisResouceBefore->shared_mem = _get_rusage_resource(r->pool, pRcheckerDirConf->mem_process_type, "shared_mem");
    }

    if (match == 0) {
#ifdef __MOD_DEBUG__
        RCHECKER_DEBUG_SYSLOG("before_resource_checker: ", "no match dir end", r->pool);
#endif
        return OK;
    }

#ifdef __MOD_DEBUG__
    fs_debug_rchecker_log_buf = ap_psprintf(r->pool,
            "pAnalysisResouceBefore->cpu_utime=(%lf[sec]) pAnalysisResouceBefore->cpu_stime=(%lf[sec]) pAnalysisResouceBefore->shared_mem=(%lf[kb])"
            , pAnalysisResouceBefore->cpu_utime
            , pAnalysisResouceBefore->cpu_stime
            , pAnalysisResouceBefore->shared_mem
    );
    RCHECKER_DEBUG_SYSLOG("before_resource_checker: ", fs_debug_rchecker_log_buf, r->pool);
#endif

#ifdef __MOD_DEBUG__
    RCHECKER_DEBUG_SYSLOG("before_resource_checker: ", "end", r->pool);
#endif

    return OK;
}


/* ------------------------------------------------- */
/* --- log transantion (ap_hook_log_transaction) --- */
/* ------------------------------------------------- */
static int after_resource_checker(request_rec *r)
{
    int match;
    struct stat sb;
    RESOURCE_DATA *pAnalysisResouceAfter;
    pAnalysisResouceAfter = (RESOURCE_DATA *)ap_pcalloc(r->pool, sizeof(RESOURCE_DATA));
    RESOURCE_DATA *pAnalysisResouceNow;
    pAnalysisResouceNow = (RESOURCE_DATA *)ap_pcalloc(r->pool, sizeof(RESOURCE_DATA));

    RCHECKER_D_CONF *pRcheckerDirConf;
    pRcheckerDirConf = (RCHECKER_D_CONF *)ap_get_module_config(r->per_dir_config, &rchecker_module);

    ACCESS_INFO *pAccessInfoData;
    pAccessInfoData = (ACCESS_INFO *)ap_pcalloc(r->pool, sizeof(ACCESS_INFO));

#ifdef __MOD_DEBUG__
    RCHECKER_DEBUG_SYSLOG("after_resource_checker: ", "start", r->pool);
#endif


    if (rchecker_initialized == 0) { 
        return OK;
    }

#ifdef __MOD_APACHE1__
    if (r->main) {
        return OK;
    }
#endif

#ifdef __MOD_APACHE2__
    if (r->main && (stat(r->filename, &sb) == -1) && errno == ENOENT) {
       return OK;
    }
#endif

    pAccessInfoData->access_uri      = r->uri;
    pAccessInfoData->access_file     = r->filename;
    pAccessInfoData->access_src_ip   = r->connection->remote_ip;
    pAccessInfoData->access_dst_host = r->server->server_hostname;

#ifdef __MOD_DEBUG__
    fs_debug_rchecker_log_buf = ap_psprintf(r->pool,
            "pRcheckerDirConf: pRcheckerDirConf->target_dir=(%s) pRcheckerDirConf->cpu_utime=(%lf) pRcheckerDirConf->cpu_stime=(%lf) pRcheckerDirConf->shared_mem=(%lf)"
            , pRcheckerDirConf->target_dir
            , pRcheckerDirConf->cpu_utime
            , pRcheckerDirConf->cpu_stime
            , pRcheckerDirConf->shared_mem
    );
    RCHECKER_DEBUG_SYSLOG("after_resource_checker: ", fs_debug_rchecker_log_buf, r->pool);
#endif

    // threashould check  

    match = 0;
    pAnalysisResouceAfter->cpu_utime  = INITIAL_VALUE;
    pAnalysisResouceAfter->cpu_stime  = INITIAL_VALUE;
    pAnalysisResouceAfter->shared_mem = INITIAL_VALUE;

    if (pRcheckerDirConf->cpu_utime > INITIAL_VALUE) {
        match = 1;
        pAnalysisResouceAfter->cpu_utime  = _get_rusage_resource(r->pool, pRcheckerDirConf->utime_process_type, "cpu_utime");
    } 

    if (pRcheckerDirConf->cpu_stime > INITIAL_VALUE) {
        match = 1;
        pAnalysisResouceAfter->cpu_stime  = _get_rusage_resource(r->pool, pRcheckerDirConf->stime_process_type, "cpu_stime");
    }

    if (pRcheckerDirConf->shared_mem > INITIAL_VALUE) {
        match = 1;
        pAnalysisResouceAfter->shared_mem = _get_rusage_resource(r->pool, pRcheckerDirConf->mem_process_type, "shared_mem");
    }

    if (match == 0) {
#ifdef __MOD_DEBUG__
        RCHECKER_DEBUG_SYSLOG("after_resource_checker: ", "no match dir end", r->pool);
#endif
        return OK;
    }

#ifdef __MOD_DEBUG__
    fs_debug_rchecker_log_buf = ap_psprintf(r->pool,
            "file=(%s) pAnalysisResouceBefore->cpu_utime=(%lf[sec]) pAnalysisResouceBefore->cpu_stime=(%lf[sec]) pAnalysisResouceBefore->shared_mem=(%lf[MB])"
            , pAccessInfoData->access_file
            , pAnalysisResouceBefore->cpu_utime
            , pAnalysisResouceBefore->cpu_stime
            , pAnalysisResouceBefore->shared_mem
    );
    RCHECKER_DEBUG_SYSLOG("after_resource_checker: ", fs_debug_rchecker_log_buf, r->pool);
#endif

#ifdef __MOD_DEBUG__
    fs_debug_rchecker_log_buf = ap_psprintf(r->pool,
            "file=(%s) pAnalysisResouceAfter->cpu_utime=(%lf[sec]) pAnalysisResouceAfter->cpu_stime=(%lf[sec]) pAnalysisResouceAfter->shared_mem=(%lf[MB])"
            , pAccessInfoData->access_file
            , pAnalysisResouceAfter->cpu_utime
            , pAnalysisResouceAfter->cpu_stime
            , pAnalysisResouceAfter->shared_mem
    );
    RCHECKER_DEBUG_SYSLOG("after_resource_checker: ", fs_debug_rchecker_log_buf, r->pool);
#endif

    pAnalysisResouceNow->cpu_utime  = pAnalysisResouceAfter->cpu_utime - pAnalysisResouceBefore->cpu_utime;
    pAnalysisResouceNow->cpu_stime =  pAnalysisResouceAfter->cpu_stime - pAnalysisResouceBefore->cpu_stime;
    pAnalysisResouceNow->shared_mem = pAnalysisResouceAfter->shared_mem - pAnalysisResouceBefore->shared_mem;

#ifdef __MOD_DEBUG__
    fs_debug_rchecker_log_buf = ap_psprintf(r->pool,
            "file=(%s) pAnalysisResouceNow->cpu_utime=(%lf[sec]) pAnalysisResouceNow->cpu_stime=(%lf[sec]) pAnalysisResouceNow->shared_mem=(%lf[MB])"
            , pAccessInfoData->access_file
            , pAnalysisResouceNow->cpu_utime
            , pAnalysisResouceNow->cpu_stime
            , pAnalysisResouceNow->shared_mem
    );
    RCHECKER_DEBUG_SYSLOG("after_resource_checker: ", fs_debug_rchecker_log_buf, r->pool);
#endif

    if (pRcheckerDirConf->cpu_utime > INITIAL_VALUE && pAnalysisResouceNow->cpu_utime >= pRcheckerDirConf->cpu_utime) {
        _mod_rchecker_logging(pAnalysisResouceNow->cpu_utime
            , pRcheckerDirConf->cpu_utime
            , pRcheckerDirConf->utime_process_type
            , pRcheckerDirConf
            , pAccessInfoData
            , "RCHECKER: [ RCheckUCPU(sec) ="
            , r->pool
        );
    }

    if (pRcheckerDirConf->cpu_stime > INITIAL_VALUE && pAnalysisResouceNow->cpu_stime >= pRcheckerDirConf->cpu_stime) {
        _mod_rchecker_logging(pAnalysisResouceNow->cpu_stime
            , pRcheckerDirConf->cpu_stime
            , pRcheckerDirConf->stime_process_type
            , pRcheckerDirConf
            , pAccessInfoData
            , "RCHECKER: [ RCheckSCPU(sec) ="
            , r->pool
        );
    }

    if (pRcheckerDirConf->shared_mem > INITIAL_VALUE && pAnalysisResouceNow->shared_mem >= pRcheckerDirConf->shared_mem) {
        _mod_rchecker_logging(pAnalysisResouceNow->shared_mem
            , pRcheckerDirConf->shared_mem
            , pRcheckerDirConf->mem_process_type
            , pRcheckerDirConf
            , pAccessInfoData
            , "RCHECKER: [ RCheckMEM(MB) ="
            , r->pool
        );
    }

#ifdef __MOD_DEBUG__
    RCHECKER_DEBUG_SYSLOG("after_resource_checker: ", "end", r->pool);
#endif

    return OK;
}

/* ------------------- */
/* --- Command_rec --- */
/* ------------------- */
static const command_rec rchecker_cmds[] = {
    AP_INIT_TAKE2("RCheckUCPU", (void *)set_cpu_utime_resouce, NULL, OR_ALL, "Set Resource Checker User CPU Time."),
    AP_INIT_TAKE2("RCheckSCPU", (void *)set_cpu_stime_resouce, NULL, OR_ALL, "Set Resource Checker System CPU Time."),
    AP_INIT_TAKE2("RCheckMEM", (void *)set_shared_mem_resouce, NULL, OR_ALL, "Set Resource Checker Process Memory."),
    {NULL}
};

/* -------------- */
/* --- Module --- */
/* -------------- */
static void rchecker_register_hooks(apr_pool_t *p)
{
    ap_hook_post_config((void*)rchecker_init, NULL, NULL, APR_HOOK_MIDDLE);
    ap_hook_access_checker(before_resource_checker, NULL, NULL, APR_HOOK_LAST);
    ap_hook_log_transaction(after_resource_checker, NULL, NULL, APR_HOOK_MIDDLE);
}

module AP_MODULE_DECLARE_DATA rchecker_module = {
    STANDARD20_MODULE_STUFF,
    (void*)rchecker_create_dir_config,      /* create per-dir    config structures */
    NULL,                                   /* merge  per-dir    config structures */
    NULL,                                   /* create per-server config structures */
    NULL,                                   /* merge  per-server config structures */
    rchecker_cmds,                          /* table of config file commands       */
    rchecker_register_hooks                 /* register hooks                      */
};
