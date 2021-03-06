/*
 * Copyright 2004-2019 the Pacemaker project contributors
 *
 * The version control history for this file may have further details.
 *
 * This source code is licensed under the GNU General Public License version 2
 * or later (GPLv2+) WITHOUT ANY WARRANTY.
 */

#include <crm_internal.h>

#include <sys/param.h>

#include <crm/crm.h>

#include <stdio.h>
#include <sys/types.h>
#include <unistd.h>

#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>

#include <crm/msg_xml.h>
#include <crm/common/xml.h>
#include <crm/common/mainloop.h>
#include <crm/cib/internal.h>

#include <crm/common/ipc.h>
#include <crm/pengine/status.h>
#include <../lib/pengine/unpack.h>

#include <crm/cib.h>

#ifdef HAVE_GETOPT_H
#  include <getopt.h>
#endif

static int max_failures = 30;

static gboolean log_diffs = FALSE;
static gboolean log_updates = FALSE;

static GMainLoop *mainloop = NULL;
void usage(const char *cmd, crm_exit_t exit_status);
void cib_connection_destroy(gpointer user_data);

void cibmon_shutdown(int nsig);
void cibmon_diff(const char *event, xmlNode * msg);

static cib_t *cib = NULL;
static xmlNode *cib_copy = NULL;

#define OPTARGS	"V?m:du"

int
main(int argc, char **argv)
{
    int argerr = 0;
    int flag;
    int attempts = 0;
    int rc = pcmk_ok;

#ifdef HAVE_GETOPT_H
    int option_index = 0;

    static struct option long_options[] = {
        /* Top-level Options */
        {"verbose", 0, 0, 'V'},
        {"help", 0, 0, '?'},
        {"log-diffs", 0, 0, 'd'},
        {"log-updates", 0, 0, 'u'},
        {"max-conn-fail", 1, 0, 'm'},
        {0, 0, 0, 0}
    };
#endif

    crm_log_cli_init("cibmon");

    crm_signal_handler(SIGTERM, cibmon_shutdown);

    while (1) {
#ifdef HAVE_GETOPT_H
        flag = getopt_long(argc, argv, OPTARGS, long_options, &option_index);
#else
        flag = getopt(argc, argv, OPTARGS);
#endif
        if (flag == -1)
            break;

        switch (flag) {
            case 'V':
                crm_bump_log_level(argc, argv);
                break;
            case '?':
                usage(crm_system_name, CRM_EX_OK);
                break;
            case 'd':
                log_diffs = TRUE;
                break;
            case 'u':
                log_updates = TRUE;
                break;
            case 'm':
                max_failures = crm_parse_int(optarg, "30");
                break;
            default:
                printf("Argument code 0%o (%c)" " is not (?yet?) supported\n", flag, flag);
                ++argerr;
                break;
        }
    }

    if (optind < argc) {
        printf("non-option ARGV-elements: ");
        while (optind < argc)
            printf("%s ", argv[optind++]);
        printf("\n");
    }

    if (optind > argc) {
        ++argerr;
    }

    if (argerr) {
        usage(crm_system_name, CRM_EX_USAGE);
    }

    cib = cib_new();

    do {
        sleep(1);
        rc = cib->cmds->signon(cib, crm_system_name, cib_query);

    } while (rc == -ENOTCONN && attempts++ < max_failures);

    if (rc != pcmk_ok) {
        crm_err("Signon to CIB failed: %s", pcmk_strerror(rc));
        goto fail;
    }

    crm_debug("Setting dnotify");
    rc = cib->cmds->set_connection_dnotify(cib, cib_connection_destroy);
    if (rc != pcmk_ok) {
        crm_err("Failed to set dnotify callback: %s", pcmk_strerror(rc));
        goto fail;
    }

    crm_debug("Setting diff callback");
    rc = cib->cmds->add_notify_callback(cib, T_CIB_DIFF_NOTIFY, cibmon_diff);
    if (rc != pcmk_ok) {
        crm_err("Failed to set diff callback: %s", pcmk_strerror(rc));
        goto fail;
    }

    mainloop = g_main_loop_new(NULL, FALSE);
    crm_info("Starting mainloop");
    g_main_loop_run(mainloop);
    crm_trace("%s exiting normally", crm_system_name);
    fflush(stderr);
    return CRM_EX_OK;

fail:
    crm_err("Setup failed, could not monitor CIB actions");
    return CRM_EX_ERROR;
}

void
usage(const char *cmd, crm_exit_t exit_status)
{
    FILE *stream;

    stream = (exit_status == CRM_EX_OK)? stdout : stderr;
    fflush(stream);

    crm_exit(exit_status);
}

void
cib_connection_destroy(gpointer user_data)
{
    cib_t *conn = user_data;

    crm_err("Connection to the CIB terminated... exiting");
    conn->cmds->signoff(conn);  /* Ensure IPC is cleaned up */
    g_main_loop_quit(mainloop);
    return;
}

void
cibmon_diff(const char *event, xmlNode * msg)
{
    int rc = -1;
    const char *op = NULL;
    unsigned int log_level = LOG_INFO;

    xmlNode *diff = NULL;
    xmlNode *cib_last = NULL;
    xmlNode *update = get_message_xml(msg, F_CIB_UPDATE);

    if (msg == NULL) {
        crm_err("NULL update");
        return;
    }

    crm_element_value_int(msg, F_CIB_RC, &rc);
    op = crm_element_value(msg, F_CIB_OPERATION);
    diff = get_message_xml(msg, F_CIB_UPDATE_RESULT);

    if (rc < pcmk_ok) {
        log_level = LOG_WARNING;
        do_crm_log(log_level, "[%s] %s ABORTED: %s", event, op, pcmk_strerror(rc));
        return;
    }

    if (log_diffs) {
        xml_log_patchset(log_level, op, diff);
    }

    if (log_updates && update != NULL) {
        crm_log_xml_trace(update, "raw_update");
    }

    if (cib_copy != NULL) {
        cib_last = cib_copy;
        cib_copy = NULL;
        rc = cib_process_diff(op, cib_force_diff, NULL, NULL, diff, cib_last, &cib_copy, NULL);

        if (rc != pcmk_ok) {
            crm_debug("Update didn't apply, requesting full copy: %s", pcmk_strerror(rc));
            free_xml(cib_copy);
            cib_copy = NULL;
        }
    }

    if (cib_copy == NULL) {
        rc = cib->cmds->query(cib, NULL, &cib_copy, cib_scope_local | cib_sync_call);
    }

    if(rc == -EACCES) {
        crm_exit(CRM_EX_INSUFFICIENT_PRIV);
    }

    free_xml(cib_last);
}

void
cibmon_shutdown(int nsig)
{
    crm_exit(CRM_EX_OK);
}
