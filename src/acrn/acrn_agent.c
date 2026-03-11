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
#include "virlog.h"
#include "virstring.h"
#include "virutil.h"

#define VIR_FROM_THIS VIR_FROM_ACRN

VIR_LOG_INIT("acrn.acrn_agent");

#define ACRN_AGENT_REPLY_TIMEOUT_MS 500

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
