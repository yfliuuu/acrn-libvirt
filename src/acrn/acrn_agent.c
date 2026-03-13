/*
 * acrn_agent.c: interaction with guest agent for acrn guests
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library.  If not, see
 * <http://www.gnu.org/licenses/>.
 */

#include <config.h>

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "acrn_agent.h"
#include "virerror.h"
#include "virfile.h"
#include "virjson.h"
#include "virlog.h"
#include "virstring.h"
#include "virtypedparam.h"
#include "virutil.h"

#define VIR_FROM_THIS VIR_FROM_ACRN

VIR_LOG_INIT("acrn.acrn_agent");

#define ACRN_AGENT_REPLY_TIMEOUT_MS 500
#define ACRN_AGENT_COMMAND_TIMEOUT_MS 5000
#define ACRN_AGENT_MAX_RESPONSE (1024 * 1024)

static int
acrnAgentOpenUnix(const char *socketpath)
{
    struct sockaddr_un addr = { 0 };
    int fd;

    if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) {
        virReportSystemError(errno, "%s",
                             _("failed to create guest agent socket"));
        return -1;
    }

    if (virSetCloseExec(fd) < 0) {
        virReportSystemError(errno, "%s",
                             _("unable to set close-on-exec flag on guest agent socket"));
        goto error;
    }

    addr.sun_family = AF_UNIX;
    if (virStrcpyStatic(addr.sun_path, socketpath) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("guest agent socket path %1$s is too long"),
                       socketpath);
        goto error;
    }

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        virReportError(VIR_ERR_AGENT_UNRESPONSIVE,
                       _("QEMU guest agent is not connected (%1$s)"),
                       socketpath);
        goto error;
    }

    return fd;

 error:
    VIR_FORCE_CLOSE(fd);
    return -1;
}

static const char *
acrnAgentSuspendTargetToCommand(unsigned int target)
{
    switch (target) {
    case VIR_NODE_SUSPEND_TARGET_MEM:
        return "guest-suspend-ram";
    case VIR_NODE_SUSPEND_TARGET_DISK:
        return "guest-suspend-disk";
    case VIR_NODE_SUSPEND_TARGET_HYBRID:
        return "guest-suspend-hybrid";
    default:
        return NULL;
    }
}

static int
acrnAgentWriteAll(int fd,
                  const char *buf,
                  size_t len)
{
    size_t offset = 0;

    while (offset < len) {
        ssize_t written = write(fd, buf + offset, len - offset);

        if (written < 0) {
            if (errno == EINTR)
                continue;

            virReportSystemError(errno, "%s",
                                 _("failed to write guest agent command"));
            return -1;
        }

        offset += written;
    }

    return 0;
}

static int
acrnAgentReadReply(int fd)
{
    g_autofree char *reply = g_new0(char, 4096);
    struct pollfd pfd = {
        .fd = fd,
        .events = POLLIN | POLLERR | POLLHUP,
        .revents = 0,
    };
    ssize_t got;
    int rv;

    rv = poll(&pfd, 1, ACRN_AGENT_REPLY_TIMEOUT_MS);
    if (rv < 0) {
        virReportSystemError(errno, "%s",
                             _("failed to wait for guest agent reply"));
        return -1;
    }

    /* Guest may suspend before sending any reply; treat timeout as success. */
    if (rv == 0)
        return 0;

    if (pfd.revents & POLLHUP)
        return 0;

    if (pfd.revents & POLLERR)
        return 0;

    if (!(pfd.revents & POLLIN))
        return 0;

    got = read(fd, reply, 4095);
    if (got < 0) {
        if (errno == EINTR)
            return 0;

        virReportSystemError(errno, "%s",
                             _("failed to read guest agent reply"));
        return -1;
    }

    if (got == 0)
        return 0;

    reply[got] = '\0';

    if (strstr(reply, "\"error\"")) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("guest agent returned an error: %1$s"),
                       reply);
        return -1;
    }

    return 0;
}


static int
acrnAgentExtractReply(const char *line,
                      virJSONValue **reply)
{
    g_autoptr(virJSONValue) obj = NULL;

    if (!(obj = virJSONValueFromString(line)))
        return 0;

    if (virJSONValueGetType(obj) != VIR_JSON_TYPE_OBJECT)
        return 0;

    if (virJSONValueObjectHasKey(obj, "return") ||
        virJSONValueObjectHasKey(obj, "error")) {
        *reply = g_steal_pointer(&obj);
        return 1;
    }

    return 0;
}


static int
acrnAgentReadCommandReply(int fd,
                          virJSONValue **reply)
{
    g_autoptr(GString) pending = g_string_new(NULL);
    char chunk[1024];

    *reply = NULL;

    while (true) {
        struct pollfd pfd = {
            .fd = fd,
            .events = POLLIN | POLLERR | POLLHUP,
            .revents = 0,
        };
        ssize_t got;
        int rv;

        rv = poll(&pfd, 1, ACRN_AGENT_COMMAND_TIMEOUT_MS);
        if (rv < 0) {
            if (errno == EINTR)
                continue;

            virReportSystemError(errno, "%s",
                                 _("failed to wait for guest agent reply"));
            return -1;
        }

        if (rv == 0) {
            virReportError(VIR_ERR_AGENT_UNRESPONSIVE, "%s",
                           _("guest agent command timed out"));
            return -1;
        }

        if (pfd.revents & POLLERR) {
            virReportError(VIR_ERR_AGENT_UNRESPONSIVE, "%s",
                           _("guest agent connection error"));
            return -1;
        }

        if (!(pfd.revents & (POLLIN | POLLHUP)))
            continue;

        got = read(fd, chunk, sizeof(chunk) - 1);
        if (got < 0) {
            if (errno == EINTR)
                continue;

            virReportSystemError(errno, "%s",
                                 _("failed to read guest agent reply"));
            return -1;
        }

        if (got > 0) {
            chunk[got] = '\0';
            g_string_append(pending, chunk);

            if (pending->len > ACRN_AGENT_MAX_RESPONSE) {
                virReportError(VIR_ERR_INTERNAL_ERROR,
                               _("guest agent reply exceeded maximum size of %1$u bytes"),
                               ACRN_AGENT_MAX_RESPONSE);
                return -1;
            }
        }

        while (true) {
            char *newline = strchr(pending->str, '\n');
            g_autofree char *line = NULL;

            if (!newline)
                break;

            line = g_strndup(pending->str, newline - pending->str);
            g_string_erase(pending, 0, newline - pending->str + 1);

            if (line[0] == '\0')
                continue;

            if (acrnAgentExtractReply(line, reply) == 1)
                return 0;
        }

        if (pfd.revents & POLLHUP) {
            if (pending->len > 0 &&
                acrnAgentExtractReply(pending->str, reply) == 1)
                return 0;

            virReportError(VIR_ERR_AGENT_UNRESPONSIVE, "%s",
                           _("guest agent closed connection without a reply"));
            return -1;
        }
    }
}


static const char *
acrnAgentErrorDetail(virJSONValue *error)
{
    const char *detail = virJSONValueObjectGetString(error, "desc");
    const char *klass = virJSONValueObjectGetString(error, "class");

    if (detail)
        return detail;

    if (klass)
        return klass;

    return "unknown guest agent error";
}


/* Returns: 0 on success
 *          -2 when command is unsupported and 'report_unsupported' is false
 *          -1 on all other errors
 */
static int
acrnAgentCheckError(const char *command,
                    virJSONValue *reply,
                    bool report_unsupported)
{
    if (virJSONValueObjectHasKey(reply, "error")) {
        virJSONValue *error = virJSONValueObjectGet(reply, "error");
        const char *klass = NULL;

        if (!error) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("unable to execute guest agent command '%1$s'"),
                           command);
            return -1;
        }

        klass = virJSONValueObjectGetString(error, "class");
        if (!report_unsupported &&
            (STREQ_NULLABLE(klass, "CommandNotFound") ||
             STREQ_NULLABLE(klass, "CommandDisabled"))) {
            return -2;
        }

        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("unable to execute guest agent command '%1$s': %2$s"),
                       command, acrnAgentErrorDetail(error));
        return -1;
    }

    if (!virJSONValueObjectHasKey(reply, "return")) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("guest agent command '%1$s' returned no data"),
                       command);
        return -1;
    }

    return 0;
}


static int
acrnAgentCommand(virDomainChrDef *agentChannel,
                 const char *command,
                 bool report_unsupported,
                 virJSONValue **reply)
{
    g_autofree char *msg = NULL;
    g_autoptr(virJSONValue) response = NULL;
    int fd = -1;
    int ret = -1;

    *reply = NULL;

    if (!agentChannel || !agentChannel->source) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("invalid guest agent configuration"));
        return -1;
    }

    fd = acrnAgentOpenUnix(agentChannel->source->data.nix.path);
    if (fd < 0)
        return -1;

    msg = g_strdup_printf("{\"execute\":\"%s\"}\n", command);

    if (acrnAgentWriteAll(fd, msg, strlen(msg)) < 0)
        goto cleanup;

    if (acrnAgentReadCommandReply(fd, &response) < 0)
        goto cleanup;

    ret = acrnAgentCheckError(command, response, report_unsupported);
    if (ret < 0)
        goto cleanup;

    *reply = g_steal_pointer(&response);
    ret = 0;

 cleanup:
    VIR_FORCE_CLOSE(fd);
    return ret;
}

virDomainChrDef *
virAcrnFindAgentConfig(virDomainDef *def)
{
    size_t i;

    for (i = 0; i < def->nchannels; i++) {
        virDomainChrDef *channel = def->channels[i];

        if (channel->targetType != VIR_DOMAIN_CHR_CHANNEL_TARGET_TYPE_VIRTIO)
            continue;

        if (STREQ_NULLABLE(channel->target.name, "org.qemu.guest_agent.0"))
            return channel;
    }

    return NULL;
}

int
virAcrnAgentSuspend(virDomainObj *vm,
                    virDomainChrDef *agentChannel,
                    unsigned int target)
{
    const char *command;
    g_autofree char *msg = NULL;
    int fd = -1;
    int ret = -1;

    if (!vm || !agentChannel || !agentChannel->source) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("invalid guest agent configuration"));
        return -1;
    }

    if (!(command = acrnAgentSuspendTargetToCommand(target))) {
        virReportError(VIR_ERR_INVALID_ARG,
                       _("Unknown suspend target: %1$u"),
                       target);
        return -1;
    }

    fd = acrnAgentOpenUnix(agentChannel->source->data.nix.path);
    if (fd < 0)
        return -1;

    msg = g_strdup_printf("{\"execute\":\"%s\"}\n", command);

    if (acrnAgentWriteAll(fd, msg, strlen(msg)) < 0)
        goto cleanup;

    ret = acrnAgentReadReply(fd);

 cleanup:
    VIR_FORCE_CLOSE(fd);
    return ret;
}


int
virAcrnAgentGetUsers(virDomainObj *vm,
                     virDomainChrDef *agentChannel,
                     virTypedParameterPtr *params,
                     int *nparams,
                     int *maxparams,
                     bool report_unsupported)
{
    g_autoptr(virJSONValue) reply = NULL;
    virJSONValue *data = NULL;
    size_t ndata;
    size_t i;
    int rc;

    if (!vm || !agentChannel || !params || !nparams || !maxparams) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("invalid guest agent configuration"));
        return -1;
    }

    if ((rc = acrnAgentCommand(agentChannel, "guest-get-users",
                               report_unsupported, &reply)) < 0)
        return rc;

    if (!(data = virJSONValueObjectGetArray(reply, "return"))) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("guest-get-users reply was missing return data"));
        return -1;
    }

    ndata = virJSONValueArraySize(data);

    if (virTypedParamsAddUInt(params, nparams, maxparams,
                              "user.count", ndata) < 0)
        return -1;

    for (i = 0; i < ndata; i++) {
        virJSONValue *entry = virJSONValueArrayGet(data, i);
        char param_name[VIR_TYPED_PARAM_FIELD_LENGTH];
        const char *strvalue;
        double logintime;

        if (!entry) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("array element missing in guest-get-users return value"));
            return -1;
        }

        if (!(strvalue = virJSONValueObjectGetString(entry, "user"))) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("'user' missing in reply of guest-get-users"));
            return -1;
        }

        g_snprintf(param_name, VIR_TYPED_PARAM_FIELD_LENGTH,
                   "user.%zu.name", i);
        if (virTypedParamsAddString(params, nparams, maxparams,
                                    param_name, strvalue) < 0)
            return -1;

        if ((strvalue = virJSONValueObjectGetString(entry, "domain"))) {
            g_snprintf(param_name, VIR_TYPED_PARAM_FIELD_LENGTH,
                       "user.%zu.domain", i);
            if (virTypedParamsAddString(params, nparams, maxparams,
                                        param_name, strvalue) < 0)
                return -1;
        }

        if (virJSONValueObjectGetNumberDouble(entry, "login-time", &logintime) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("'login-time' missing in reply of guest-get-users"));
            return -1;
        }

        g_snprintf(param_name, VIR_TYPED_PARAM_FIELD_LENGTH,
                   "user.%zu.login-time", i);
        if (virTypedParamsAddULLong(params, nparams, maxparams,
                                    param_name,
                                    (unsigned long long)(logintime * 1000)) < 0)
            return -1;
    }

    return 0;
}


int
virAcrnAgentGetOSInfo(virDomainObj *vm,
                      virDomainChrDef *agentChannel,
                      virTypedParameterPtr *params,
                      int *nparams,
                      int *maxparams,
                      bool report_unsupported)
{
    g_autoptr(virJSONValue) reply = NULL;
    virJSONValue *data = NULL;
    int rc;

    if (!vm || !agentChannel || !params || !nparams || !maxparams) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("invalid guest agent configuration"));
        return -1;
    }

    if ((rc = acrnAgentCommand(agentChannel, "guest-get-osinfo",
                               report_unsupported, &reply)) < 0)
        return rc;

    if (!(data = virJSONValueObjectGetObject(reply, "return"))) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("guest-get-osinfo reply was missing return data"));
        return -1;
    }

#define ACRN_OSINFO_ADD_PARAM(agent_string_, param_string_) \
    do { \
        const char *result; \
        if ((result = virJSONValueObjectGetString(data, agent_string_))) { \
            if (virTypedParamsAddString(params, nparams, maxparams, \
                                        param_string_, result) < 0) { \
                return -1; \
            } \
        } \
    } while (0)

    ACRN_OSINFO_ADD_PARAM("id", "os.id");
    ACRN_OSINFO_ADD_PARAM("name", "os.name");
    ACRN_OSINFO_ADD_PARAM("pretty-name", "os.pretty-name");
    ACRN_OSINFO_ADD_PARAM("version", "os.version");
    ACRN_OSINFO_ADD_PARAM("version-id", "os.version-id");
    ACRN_OSINFO_ADD_PARAM("machine", "os.machine");
    ACRN_OSINFO_ADD_PARAM("variant", "os.variant");
    ACRN_OSINFO_ADD_PARAM("variant-id", "os.variant-id");
    ACRN_OSINFO_ADD_PARAM("kernel-release", "os.kernel-release");
    ACRN_OSINFO_ADD_PARAM("kernel-version", "os.kernel-version");

#undef ACRN_OSINFO_ADD_PARAM

    return 0;
}


int
virAcrnAgentGetTimezone(virDomainObj *vm,
                        virDomainChrDef *agentChannel,
                        virTypedParameterPtr *params,
                        int *nparams,
                        int *maxparams,
                        bool report_unsupported)
{
    g_autoptr(virJSONValue) reply = NULL;
    virJSONValue *data = NULL;
    const char *name;
    int offset;
    int rc;

    if (!vm || !agentChannel || !params || !nparams || !maxparams) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("invalid guest agent configuration"));
        return -1;
    }

    if ((rc = acrnAgentCommand(agentChannel, "guest-get-timezone",
                               report_unsupported, &reply)) < 0)
        return rc;

    if (!(data = virJSONValueObjectGetObject(reply, "return"))) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("guest-get-timezone reply was missing return data"));
        return -1;
    }

    if ((name = virJSONValueObjectGetString(data, "zone")) &&
        virTypedParamsAddString(params, nparams, maxparams,
                                "timezone.name", name) < 0)
        return -1;

    if ((virJSONValueObjectGetNumberInt(data, "offset", &offset)) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("'offset' missing in reply of guest-get-timezone"));
        return -1;
    }

    if (virTypedParamsAddInt(params, nparams, maxparams,
                             "timezone.offset", offset) < 0)
        return -1;

    return 0;
}


int
virAcrnAgentGetHostname(virDomainObj *vm,
                        virDomainChrDef *agentChannel,
                        char **hostname,
                        bool report_unsupported)
{
    g_autoptr(virJSONValue) reply = NULL;
    virJSONValue *data = NULL;
    const char *result = NULL;
    int rc;

    if (!vm || !agentChannel || !hostname) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("invalid guest agent configuration"));
        return -1;
    }

    *hostname = NULL;

    if ((rc = acrnAgentCommand(agentChannel, "guest-get-host-name",
                               report_unsupported, &reply)) < 0)
        return rc;

    if (!(data = virJSONValueObjectGet(reply, "return"))) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("malformed return value"));
        return -1;
    }

    if (!(result = virJSONValueObjectGetString(data, "host-name"))) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("'host-name' missing in guest-get-host-name reply"));
        return -1;
    }

    *hostname = g_strdup(result);
    return 0;
}
