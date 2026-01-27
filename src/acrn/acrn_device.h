#ifndef __ACRN_DEVICE_H__
#define __ACRN_DEVICE_H__

#include "domain_conf.h"
#include "acrn_driver.h"

int acrnDomainAssignAddresses(virDomainDefPtr def);
#endif /* __ACRN_DEVICE_H__ */
