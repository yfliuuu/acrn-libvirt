#ifndef __ACRN_DRIVER_H__
#define __ACRN_DRIVER_H__

#include "virbitmap.h"
#include "virbuffer.h"
#include "virdomainobjlist.h"
#include "virerror.h"
#include "viralloc.h"
#include "virconf.h"
#include "virutil.h"
#include "virhostcpu.h"
#include "vircommand.h"
#include "virthread.h"
#include "virstring.h"
#include "virfile.h"
#include "virhostdev.h"
#include "virnodesuspend.h"
#include "virnetdevbridge.h"
#include "virnetdevtap.h"
#include "virfdstream.h"
#include "virlog.h"
#include "virpci.h"
#include "virjson.h"
#include "node_device_conf.h"
#include "object_event.h"
#include "domain_addr.h"

typedef virBitmap *virBitmapPtr;
typedef virBuffer *virBufferPtr;
typedef virCaps *virCapsPtr;
typedef virCapsGuest *virCapsGuestPtr;
typedef virCommand *virCommandPtr;
typedef virConf *virConfPtr;
typedef virCPUDef *virCPUDefPtr;
typedef virDomainDef *virDomainDefPtr;
typedef virDomainChrDef *virDomainChrDefPtr;
typedef virDomainChrSourceDef *virDomainChrSourceDefPtr;
typedef virDomainControllerDef *virDomainControllerDefPtr;
typedef virDomainDeviceInfo *virDomainDeviceInfoPtr;
typedef virDomainDeviceDef *virDomainDeviceDefPtr;
typedef virDomainDiskDef *virDomainDiskDefPtr;
typedef virDomainHostdevDef *virDomainHostdevDefPtr;
typedef virDomainHostdevSubsys *virDomainHostdevSubsysPtr;
typedef virDomainHostdevSubsysPCI *virDomainHostdevSubsysPCIPtr;
typedef virDomainHostdevSubsysUSB *virDomainHostdevSubsysUSBPtr;
typedef virDomainNetDef *virDomainNetDefPtr;
typedef virDomainObj *virDomainObjPtr;
typedef virDomainObjList *virDomainObjListPtr;
typedef virDomainPCIAddressSet *virDomainPCIAddressSetPtr;
typedef virDomainVcpuDef *virDomainVcpuDefPtr;
typedef virDomainXMLOption *virDomainXMLOptionPtr;
typedef virHostdevManager *virHostdevManagerPtr;
typedef virJSONValue *virJSONValuePtr;
typedef virPCIDevice *virPCIDevicePtr;
typedef virPCIDeviceAddress *virPCIDeviceAddressPtr;
typedef virNodeDeviceDef *virNodeDeviceDefPtr;
typedef virNodeDevCapsDef *virNodeDevCapsDefPtr;
typedef virObjectEventState *virObjectEventStatePtr;
typedef virObjectEvent *virObjectEventPtr;

int acrnRegister(void);
#endif /* __ACRN_DRIVER_H__ */
