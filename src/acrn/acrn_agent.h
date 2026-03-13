/*
 * acrn_agent.h: interaction with guest agent for acrn guests
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

#pragma once

#include "internal.h"
#include "domain_conf.h"
#include "datatypes.h"

virDomainChrDef *virAcrnFindAgentConfig(virDomainDef *def);

int virAcrnAgentSuspend(virDomainObj *vm,
                        virDomainChrDef *agentChannel,
                        unsigned int target);

int virAcrnAgentArbitraryCommand(virDomainObj *vm,
                                 virDomainChrDef *agentChannel,
                                 const char *cmd_str,
                                 char **result,
                                 int timeout);

int virAcrnAgentGetUsers(virDomainObj *vm,
                         virDomainChrDef *agentChannel,
                         virTypedParameterPtr *params,
                         int *nparams,
                         int *maxparams,
                         bool report_unsupported);

int virAcrnAgentGetOSInfo(virDomainObj *vm,
                          virDomainChrDef *agentChannel,
                          virTypedParameterPtr *params,
                          int *nparams,
                          int *maxparams,
                          bool report_unsupported);

int virAcrnAgentGetTimezone(virDomainObj *vm,
                            virDomainChrDef *agentChannel,
                            virTypedParameterPtr *params,
                            int *nparams,
                            int *maxparams,
                            bool report_unsupported);

int virAcrnAgentGetHostname(virDomainObj *vm,
                            virDomainChrDef *agentChannel,
                            char **hostname,
                            bool report_unsupported);
