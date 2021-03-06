/*
 * Copyright 2004-2020 the Pacemaker project contributors
 *
 * The version control history for this file may have further details.
 *
 * This source code is licensed under the GNU General Public License version 2
 * or later (GPLv2+) WITHOUT ANY WARRANTY.
 */

#include <crm_internal.h>

#include <sys/param.h>

#include <stdio.h>
#include <sys/types.h>
#include <unistd.h>

#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>

#include <crm/crm.h>
#include <crm/msg_xml.h>
#include <crm/common/xml.h>

#include <crm/common/mainloop.h>

#include <crm/cib.h>

static int message_timer_id = -1;
static int message_timeout_ms = 30 * 1000;

static GMainLoop *mainloop = NULL;
static crm_ipc_t *crmd_channel = NULL;
static char *admin_uuid = NULL;

gboolean do_init(void);
int do_work(void);
void crmadmin_ipc_connection_destroy(gpointer user_data);
int admin_msg_callback(const char *buffer, ssize_t length, gpointer userdata);
int do_find_node_list(xmlNode * xml_node);
gboolean admin_message_timeout(gpointer data);

static gboolean BE_VERBOSE = FALSE;
static int expected_responses = 1;
static gboolean BASH_EXPORT = FALSE;
static gboolean DO_HEALTH = FALSE;
static gboolean DO_RESET = FALSE;
static gboolean DO_RESOURCE = FALSE;
static gboolean DO_ELECT_DC = FALSE;
static gboolean DO_WHOIS_DC = FALSE;
static gboolean DO_NODE_LIST = FALSE;
static gboolean BE_SILENT = FALSE;
static gboolean DO_RESOURCE_LIST = FALSE;
static const char *crmd_operation = NULL;
static char *dest_node = NULL;
static crm_exit_t exit_code = CRM_EX_OK;
static const char *sys_to = NULL;

static pcmk__cli_option_t long_options[] = {
    // long option, argument type, storage, short option, description, flags
    {
        "help", no_argument, NULL, '?',
        "\tThis text", pcmk__option_default
    },
    {
        "version", no_argument, NULL, '$',
        "\tVersion information", pcmk__option_default
    },
    {
        "quiet", no_argument, NULL, 'q',
        "\tDisplay only the essential query information", pcmk__option_default
    },
    {
        "verbose", no_argument, NULL, 'V',
        "\tIncrease debug output", pcmk__option_default
    },
    {
        "-spacer-", no_argument, NULL, '-',
        "\nCommands:", pcmk__option_default
    },
    /* daemon options */
    {
        "status", required_argument, NULL, 'S',
        "Display the status of the specified node.", pcmk__option_default
    },
    {
        "-spacer-", no_argument, NULL, '-',
        "\n\tResult is state of node's internal finite state machine, which "
            "can be useful for debugging\n",
        pcmk__option_default
    },
    {
        "dc_lookup", no_argument, NULL, 'D',
        "Display the uname of the node co-ordinating the cluster.",
        pcmk__option_default
    },
    {
        "-spacer-", no_argument, NULL, '-',
        "\n\tThis is an internal detail rarely useful to administrators "
            "except when deciding on which node to examine the logs.\n",
        pcmk__option_default
    },
    {
        "nodes", no_argument, NULL, 'N',
        "\tDisplay the uname of all member nodes", pcmk__option_default
    },
    {
        "election", no_argument, NULL, 'E',
        "(Advanced) Start an election for the cluster co-ordinator",
        pcmk__option_default
    },
    {
        "kill", required_argument, NULL, 'K',
        "(Advanced) Stop controller (not rest of cluster stack) on "
            "specified node", pcmk__option_default
    },
    {
        "health", no_argument, NULL, 'H',
        NULL, pcmk__option_hidden
    },
    {
        "-spacer-", no_argument, NULL, '-',
        "\nAdditional Options:", pcmk__option_default
    },
    {
        XML_ATTR_TIMEOUT, required_argument, NULL, 't',
        "Time (in milliseconds) to wait before declaring the operation failed",
        pcmk__option_default
    },
    {
        "bash-export", no_argument, NULL, 'B',
        "Create Bash export entries of the form 'export uname=uuid'\n",
        pcmk__option_default
    },
    {
        "-spacer-", no_argument, NULL, '-',
        "Notes:", pcmk__option_default
    },
    {
        "-spacer-", no_argument, NULL, '-',
        " The -K and -E commands are rarely used and may be removed in "
            "future versions.",
        pcmk__option_default
    },
    { 0, 0, 0, 0 }
};

int
main(int argc, char **argv)
{
    int option_index = 0;
    int argerr = 0;
    int flag;

    crm_log_cli_init("crmadmin");
    pcmk__set_cli_options(NULL, "<command> [options]", long_options,
                          "query and manage the Pacemaker controller");
    if (argc < 2) {
        pcmk__cli_help('?', CRM_EX_USAGE);
    }

    while (1) {
        flag = pcmk__next_cli_option(argc, argv, &option_index, NULL);
        if (flag == -1)
            break;

        switch (flag) {
            case 'V':
                BE_VERBOSE = TRUE;
                crm_bump_log_level(argc, argv);
                break;
            case 't':
                message_timeout_ms = atoi(optarg);
                if (message_timeout_ms < 1) {
                    message_timeout_ms = 30 * 1000;
                }
                break;

            case '$':
            case '?':
                pcmk__cli_help(flag, CRM_EX_OK);
                break;
            case 'D':
                DO_WHOIS_DC = TRUE;
                break;
            case 'B':
                BASH_EXPORT = TRUE;
                break;
            case 'K':
                DO_RESET = TRUE;
                crm_trace("Option %c => %s", flag, optarg);
                dest_node = strdup(optarg);
                crmd_operation = CRM_OP_LOCAL_SHUTDOWN;
                break;
            case 'q':
                BE_SILENT = TRUE;
                break;
            case 'S':
                DO_HEALTH = TRUE;
                crm_trace("Option %c => %s", flag, optarg);
                dest_node = strdup(optarg);
                break;
            case 'E':
                DO_ELECT_DC = TRUE;
                break;
            case 'N':
                DO_NODE_LIST = TRUE;
                break;
            case 'H':
                DO_HEALTH = TRUE;
                break;
            default:
                printf("Argument code 0%o (%c) is not (?yet?) supported\n", flag, flag);
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
        pcmk__cli_help('?', CRM_EX_USAGE);
    }

    if (do_init()) {
        int res = 0;

        res = do_work();
        if (res > 0) {
            /* wait for the reply by creating a mainloop and running it until
             * the callbacks are invoked...
             */
            mainloop = g_main_loop_new(NULL, FALSE);
            crm_trace("Waiting for %d replies from the local CRM", expected_responses);

            message_timer_id = g_timeout_add(message_timeout_ms, admin_message_timeout, NULL);

            g_main_loop_run(mainloop);

        } else if (res < 0) {
            crm_err("No message to send");
            exit_code = CRM_EX_ERROR;
        }
    } else {
        crm_warn("Init failed, could not perform requested operations");
        exit_code = CRM_EX_UNAVAILABLE;
    }

    crm_trace("%s exiting normally", crm_system_name);
    return exit_code;
}

int
do_work(void)
{
    int ret = 1;

    /* construct the request */
    xmlNode *msg_data = NULL;
    gboolean all_is_good = TRUE;

    if (DO_HEALTH == TRUE) {
        crm_trace("Querying the system");

        sys_to = CRM_SYSTEM_DC;

        if (dest_node != NULL) {
            sys_to = CRM_SYSTEM_CRMD;
            crmd_operation = CRM_OP_PING;

            if (BE_VERBOSE) {
                expected_responses = 1;
            }

        } else {
            crm_info("Cluster-wide health not available yet");
            all_is_good = FALSE;
        }

    } else if (DO_ELECT_DC) {
        /* tell the local node to initiate an election */

        dest_node = NULL;
        sys_to = CRM_SYSTEM_CRMD;
        crmd_operation = CRM_OP_VOTE;
        ret = 0;                /* no return message */

    } else if (DO_WHOIS_DC) {
        dest_node = NULL;
        sys_to = CRM_SYSTEM_DC;
        crmd_operation = CRM_OP_PING;

    } else if (DO_NODE_LIST) {

        cib_t *the_cib = cib_new();
        xmlNode *output = NULL;

        int rc = the_cib->cmds->signon(the_cib, crm_system_name, cib_command);

        if (rc != pcmk_ok) {
            return -1;
        }

        rc = the_cib->cmds->query(the_cib, NULL, &output, cib_scope_local | cib_sync_call);
        if(rc == pcmk_ok) {
            do_find_node_list(output);

            free_xml(output);
        }
        the_cib->cmds->signoff(the_cib);
        crm_exit(crm_errno2exit(rc));

    } else if (DO_RESET) {
        /* tell dest_node to initiate the shutdown procedure
         *
         * if dest_node is NULL, the request will be sent to the
         *   local node
         */
        sys_to = CRM_SYSTEM_CRMD;
        ret = 0;                /* no return message */

    } else {
        crm_err("Unknown options");
        all_is_good = FALSE;
    }

    if (all_is_good == FALSE) {
        crm_err("Creation of request failed.  No message to send");
        return -1;
    }

/* send it */
    if (crmd_channel == NULL) {
        crm_err("The IPC connection is not valid, cannot send anything");
        return -1;
    }

    if (sys_to == NULL) {
        if (dest_node != NULL) {
            sys_to = CRM_SYSTEM_CRMD;
        } else {
            sys_to = CRM_SYSTEM_DC;
        }
    }

    {
        xmlNode *cmd = create_request(crmd_operation, msg_data, dest_node, sys_to,
                                      crm_system_name, admin_uuid);

        crm_ipc_send(crmd_channel, cmd, 0, 0, NULL);
        free_xml(cmd);
    }

    return ret;
}

void
crmadmin_ipc_connection_destroy(gpointer user_data)
{
    crm_err("Connection to controller was terminated");
    if (mainloop) {
        g_main_loop_quit(mainloop);
    } else {
        crm_exit(CRM_EX_DISCONNECT);
    }
}

struct ipc_client_callbacks crm_callbacks = {
    .dispatch = admin_msg_callback,
    .destroy = crmadmin_ipc_connection_destroy
};

gboolean
do_init(void)
{
    mainloop_io_t *source =
        mainloop_add_ipc_client(CRM_SYSTEM_CRMD, G_PRIORITY_DEFAULT, 0, NULL, &crm_callbacks);

    admin_uuid = crm_getpid_s();

    crmd_channel = mainloop_get_ipc_client(source);

    if (DO_RESOURCE || DO_RESOURCE_LIST || DO_NODE_LIST) {
        return TRUE;

    } else if (crmd_channel != NULL) {
        xmlNode *xml = create_hello_message(admin_uuid, crm_system_name, "0", "1");

        crm_ipc_send(crmd_channel, xml, 0, 0, NULL);
        return TRUE;
    }
    return FALSE;
}

static bool
validate_crm_message(xmlNode * msg, const char *sys, const char *uuid, const char *msg_type)
{
    const char *type = NULL;
    const char *crm_msg_reference = NULL;

    if (msg == NULL) {
        return FALSE;
    }

    type = crm_element_value(msg, F_CRM_MSG_TYPE);
    crm_msg_reference = crm_element_value(msg, XML_ATTR_REFERENCE);

    if (type == NULL) {
        crm_info("No message type defined.");
        return FALSE;

    } else if (msg_type != NULL && strcasecmp(msg_type, type) != 0) {
        crm_info("Expecting a (%s) message but received a (%s).", msg_type, type);
        return FALSE;
    }

    if (crm_msg_reference == NULL) {
        crm_info("No message crm_msg_reference defined.");
        return FALSE;
    }

    return TRUE;
}

int
admin_msg_callback(const char *buffer, ssize_t length, gpointer userdata)
{
    static int received_responses = 0;
    xmlNode *xml = string2xml(buffer);

    received_responses++;
    g_source_remove(message_timer_id);

    crm_log_xml_trace(xml, "ipc");

    if (xml == NULL) {
        crm_info("XML in IPC message was not valid... " "discarding.");

    } else if (validate_crm_message(xml, crm_system_name, admin_uuid, XML_ATTR_RESPONSE) == FALSE) {
        crm_trace("Message was not a CRM response. Discarding.");

    } else if (DO_HEALTH) {
        xmlNode *data = get_message_xml(xml, F_CRM_DATA);
        const char *state = crm_element_value(data, XML_PING_ATTR_CRMDSTATE);

        printf("Status of %s@%s: %s (%s)\n",
               crm_element_value(data, XML_PING_ATTR_SYSFROM),
               crm_element_value(xml, F_CRM_HOST_FROM),
               state, crm_element_value(data, XML_PING_ATTR_STATUS));

        if (BE_SILENT && state != NULL) {
            fprintf(stderr, "%s\n", state);
        }

    } else if (DO_WHOIS_DC) {
        const char *dc = crm_element_value(xml, F_CRM_HOST_FROM);

        printf("Designated Controller is: %s\n", dc);
        if (BE_SILENT && dc != NULL) {
            fprintf(stderr, "%s\n", dc);
        }
        crm_exit(CRM_EX_OK);
    }

    free_xml(xml);

    if (received_responses >= expected_responses) {
        crm_trace("Received expected number (%d) of replies, exiting normally",
                   expected_responses);
        crm_exit(CRM_EX_OK);
    }

    message_timer_id = g_timeout_add(message_timeout_ms, admin_message_timeout, NULL);
    return 0;
}

gboolean
admin_message_timeout(gpointer data)
{
    fprintf(stderr, "No messages received in %d seconds.. aborting\n",
            (int)message_timeout_ms / 1000);
    crm_err("No messages received in %d seconds", (int)message_timeout_ms / 1000);
    exit_code = CRM_EX_TIMEOUT;
    g_main_loop_quit(mainloop);
    return FALSE;
}

int
do_find_node_list(xmlNode * xml_node)
{
    int found = 0;
    xmlNode *node = NULL;
    xmlNode *nodes = get_object_root(XML_CIB_TAG_NODES, xml_node);

    for (node = __xml_first_child_element(nodes); node != NULL;
         node = __xml_next_element(node)) {

        if (crm_str_eq((const char *)node->name, XML_CIB_TAG_NODE, TRUE)) {

            if (BASH_EXPORT) {
                printf("export %s=%s\n",
                       crm_element_value(node, XML_ATTR_UNAME),
                       crm_element_value(node, XML_ATTR_ID));
            } else {
                printf("%s node: %s (%s)\n",
                       crm_element_value(node, XML_ATTR_TYPE),
                       crm_element_value(node, XML_ATTR_UNAME),
                       crm_element_value(node, XML_ATTR_ID));
            }
            found++;
        }
    }

    if (found == 0) {
        printf("NO nodes configured\n");
    }

    return found;
}
