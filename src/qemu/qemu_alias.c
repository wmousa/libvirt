/*
 * qemu_alias.c: QEMU alias manipulation
 *
 * Copyright (C) 2006-2016 Red Hat, Inc.
 * Copyright (C) 2006 Daniel P. Berrange
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

#include "qemu_alias.h"
#include "viralloc.h"
#include "virlog.h"
#include "virstring.h"
#include "virutil.h"

#define QEMU_DRIVE_HOST_PREFIX "drive-"

#define VIR_FROM_THIS VIR_FROM_QEMU

VIR_LOG_INIT("qemu.qemu_alias");

int
qemuDomainDeviceAliasIndex(const virDomainDeviceInfo *info,
                           const char *prefix)
{
    int idx;

    if (!info->alias)
        return -1;
    if (!STRPREFIX(info->alias, prefix))
        return -1;

    if (virStrToLong_i(info->alias + strlen(prefix), NULL, 10, &idx) < 0)
        return -1;

    return idx;
}


static ssize_t
qemuGetNextChrDevIndex(virDomainDef *def,
                       virDomainChrDef *chr,
                       const char *prefix)
{
    const virDomainChrDef **arrPtr;
    size_t cnt;
    size_t i;
    ssize_t idx = 0;
    const char *prefix2 = NULL;

    if (chr->deviceType == VIR_DOMAIN_CHR_DEVICE_TYPE_CONSOLE)
        prefix2 = "serial";

    virDomainChrGetDomainPtrs(def, chr->deviceType, &arrPtr, &cnt);

    for (i = 0; i < cnt; i++) {
        ssize_t thisidx;
        if (((thisidx = qemuDomainDeviceAliasIndex(&arrPtr[i]->info, prefix)) < 0) &&
            (prefix2 &&
             (thisidx = qemuDomainDeviceAliasIndex(&arrPtr[i]->info, prefix2)) < 0))
            continue;
        if (thisidx >= idx)
            idx = thisidx + 1;
    }

    return idx;
}


int
qemuAssignDeviceChrAlias(virDomainDef *def,
                         virDomainChrDef *chr,
                         ssize_t idx)
{
    const char *prefix = NULL;

    if (chr->info.alias)
        return 0;

    switch ((virDomainChrDeviceType)chr->deviceType) {
    case VIR_DOMAIN_CHR_DEVICE_TYPE_PARALLEL:
        prefix = "parallel";
        break;

    case VIR_DOMAIN_CHR_DEVICE_TYPE_SERIAL:
        prefix = "serial";
        break;

    case VIR_DOMAIN_CHR_DEVICE_TYPE_CONSOLE:
        prefix = "console";
        break;

    case VIR_DOMAIN_CHR_DEVICE_TYPE_CHANNEL:
        prefix = "channel";
        break;

    case VIR_DOMAIN_CHR_DEVICE_TYPE_LAST:
        return -1;
    }

    if (idx == -1 && (idx = qemuGetNextChrDevIndex(def, chr, prefix)) < 0)
        return -1;

    chr->info.alias = g_strdup_printf("%s%zd", prefix, idx);
    return 0;
}


int
qemuAssignDeviceControllerAlias(virDomainDef *domainDef,
                                virQEMUCaps *qemuCaps,
                                virDomainControllerDef *controller)
{
    const char *prefix = virDomainControllerTypeToString(controller->type);

    if (controller->info.alias)
        return 0;

    if (controller->type == VIR_DOMAIN_CONTROLLER_TYPE_PCI) {
        if (!virQEMUCapsHasPCIMultiBus(qemuCaps, domainDef)) {
            /* qemus that don't support multiple PCI buses have
             * hardcoded the name of their single PCI controller as
             * "pci".
             */
            controller->info.alias = g_strdup("pci");
            return 0;
        } else if (controller->model == VIR_DOMAIN_CONTROLLER_MODEL_PCIE_ROOT) {
            /* The pcie-root controller on Q35 machinetypes uses a
             * different naming convention ("pcie.0"), because it is
             * hardcoded that way in qemu.
             */
            controller->info.alias = g_strdup_printf("pcie.%d", controller->idx);
            return 0;
        }
        /* All other PCI controllers use the consistent "pci.%u"
         * (including the hardcoded pci-root controller on
         * multibus-capable qemus).
         */
        controller->info.alias = g_strdup_printf("pci.%d", controller->idx);
        return 0;
    } else if (controller->type == VIR_DOMAIN_CONTROLLER_TYPE_IDE) {
        /* for any machine based on e.g. I440FX or G3Beige, the
         * first (and currently only) IDE controller is an integrated
         * controller hardcoded with id "ide"
         */
        if (qemuDomainHasBuiltinIDE(domainDef) &&
            controller->idx == 0) {
            controller->info.alias = g_strdup("ide");
            return 0;
        }
    } else if (controller->type == VIR_DOMAIN_CONTROLLER_TYPE_SATA) {
        /* for any Q35 machine, the first SATA controller is the
         * integrated one, and it too is hardcoded with id "ide"
         */
        if (qemuDomainIsQ35(domainDef) && controller->idx == 0) {
            controller->info.alias = g_strdup("ide");
            return 0;
        }
    } else if (controller->type == VIR_DOMAIN_CONTROLLER_TYPE_USB) {
        /* first USB device is "usb", others are normal "usb%d" */
        if (controller->idx == 0) {
            controller->info.alias = g_strdup("usb");
            return 0;
        }
    } else if (controller->type == VIR_DOMAIN_CONTROLLER_TYPE_SCSI) {
        if (controller->model == VIR_DOMAIN_CONTROLLER_MODEL_SCSI_NCR53C90 &&
            controller->idx == 0) {
            controller->info.alias = g_strdup("scsi");
            return 0;
        }
    }
    /* all other controllers use the default ${type}${index} naming
     * scheme for alias/id.
     */
    controller->info.alias = g_strdup_printf("%s%d", prefix, controller->idx);
    return 0;
}


int
qemuAssignDeviceDiskAlias(virDomainDef *def,
                          virDomainDiskDef *disk,
                          virQEMUCaps *qemuCaps)
{
    qemuDomainDiskPrivate *diskPriv = QEMU_DOMAIN_DISK_PRIVATE(disk);
    const char *prefix = virDomainDiskBusTypeToString(disk->bus);
    int controllerModel = -1;

    if (!disk->info.alias) {
        if (disk->info.type == VIR_DOMAIN_DEVICE_ADDRESS_TYPE_DRIVE) {
            if (disk->bus == VIR_DOMAIN_DISK_BUS_SCSI) {
                controllerModel = qemuDomainFindSCSIControllerModel(def,
                                                                    &disk->info);
                if (controllerModel < 0)
                    return -1;
            }

            if (disk->bus != VIR_DOMAIN_DISK_BUS_SCSI ||
                controllerModel == VIR_DOMAIN_CONTROLLER_MODEL_SCSI_LSILOGIC) {
                disk->info.alias = g_strdup_printf("%s%d-%d-%d", prefix,
                                                   disk->info.addr.drive.controller,
                                                   disk->info.addr.drive.bus,
                                                   disk->info.addr.drive.unit);
            } else {
                disk->info.alias = g_strdup_printf("%s%d-%d-%d-%d", prefix,
                                                   disk->info.addr.drive.controller,
                                                   disk->info.addr.drive.bus,
                                                   disk->info.addr.drive.target,
                                                   disk->info.addr.drive.unit);
            }
        } else {
            int idx = virDiskNameToIndex(disk->dst);
            disk->info.alias = g_strdup_printf("%s-disk%d", prefix, idx);
        }
    }

    /* For -blockdev we need to know the qom names of the disk which are based
     * on the alias in qemu. While certain disk types use just the alias, some
     * need the full path into /machine/peripheral as a historical artifact.
     */
    if (!diskPriv->qomName &&
        virQEMUCapsGet(qemuCaps, QEMU_CAPS_BLOCKDEV)) {
        switch ((virDomainDiskBus) disk->bus) {
        case VIR_DOMAIN_DISK_BUS_FDC:
        case VIR_DOMAIN_DISK_BUS_IDE:
        case VIR_DOMAIN_DISK_BUS_SATA:
        case VIR_DOMAIN_DISK_BUS_SCSI:
            diskPriv->qomName = g_strdup(disk->info.alias);
            break;

        case VIR_DOMAIN_DISK_BUS_VIRTIO:
            diskPriv->qomName = g_strdup_printf("/machine/peripheral/%s/virtio-backend",
                                                disk->info.alias);
            break;

        case VIR_DOMAIN_DISK_BUS_USB:
            diskPriv->qomName = g_strdup_printf("/machine/peripheral/%s/%s.0/legacy[0]",
                                                disk->info.alias, disk->info.alias);
            break;

        case VIR_DOMAIN_DISK_BUS_XEN:
        case VIR_DOMAIN_DISK_BUS_UML:
        case VIR_DOMAIN_DISK_BUS_SD:
        case VIR_DOMAIN_DISK_BUS_LAST:
            break;
        }
    }

    return 0;
}


int
qemuAssignDeviceHostdevAlias(virDomainDef *def,
                             char **alias,
                             int idx)
{
    if (*alias)
        return 0;

    if (idx == -1) {
        size_t i;

        idx = 0;
        for (i = 0; i < def->nhostdevs; i++) {
            int thisidx;

            if ((thisidx = qemuDomainDeviceAliasIndex(def->hostdevs[i]->info, "hostdev")) < 0)
                continue; /* error just means the alias wasn't "hostdevN", but something else */
            if (thisidx >= idx)
                idx = thisidx + 1;
        }
        /* network interfaces can also have a hostdevN alias */
        for (i = 0; i < def->nnets; i++) {
            int thisidx;

            if ((thisidx = qemuDomainDeviceAliasIndex(&def->nets[i]->info, "hostdev")) < 0)
                continue;
            if (thisidx >= idx)
                idx = thisidx + 1;
        }
    }

    *alias = g_strdup_printf("hostdev%d", idx);

    return 0;
}


int
qemuAssignDeviceNetAlias(virDomainDef *def,
                         virDomainNetDef *net,
                         int idx)
{
    if (net->info.alias)
        return 0;

    /* <interface type='hostdev'> uses "hostdevN" as the alias
     * We must use "-1" as the index because the caller doesn't know
     * that we're now looking for a unique hostdevN rather than netN
     */
    if (virDomainNetResolveActualType(net) == VIR_DOMAIN_NET_TYPE_HOSTDEV)
        return qemuAssignDeviceHostdevAlias(def, &net->info.alias, -1);

    if (idx == -1) {
        size_t i;

        idx = 0;
        for (i = 0; i < def->nnets; i++) {
            int thisidx;

            if ((thisidx = qemuDomainDeviceAliasIndex(&def->nets[i]->info, "net")) < 0)
                continue; /* failure could be due to "hostdevN" */
            if (thisidx >= idx)
                idx = thisidx + 1;
        }
    }

    net->info.alias = g_strdup_printf("net%d", idx);
    return 0;
}


static int
qemuAssignDeviceFSAlias(virDomainFSDef *fss,
                        int idx)
{
    if (fss->info.alias)
        return 0;

    fss->info.alias = g_strdup_printf("fs%d", idx);
    return 0;
}


static int
qemuAssignDeviceSoundAlias(virDomainSoundDef *sound,
                           int idx)
{
    if (sound->info.alias)
        return 0;

    sound->info.alias = g_strdup_printf("sound%d", idx);
    return 0;
}


static int
qemuAssignDeviceVideoAlias(virDomainVideoDef *video,
                           int idx)
{
    if (video->info.alias)
        return 0;

    video->info.alias = g_strdup_printf("video%d", idx);
    return 0;
}


static int
qemuAssignDeviceHubAlias(virDomainHubDef *hub,
                         int idx)
{
    if (hub->info.alias)
        return 0;

    hub->info.alias = g_strdup_printf("hub%d", idx);
    return 0;
}


static int
qemuAssignDeviceSmartcardAlias(virDomainSmartcardDef *smartcard,
                               int idx)
{
    if (smartcard->info.alias)
        return 0;

    smartcard->info.alias = g_strdup_printf("smartcard%d", idx);
    return 0;
}


static int
qemuAssignDeviceMemballoonAlias(virDomainMemballoonDef *memballoon,
                                int idx)
{
    if (memballoon->info.alias)
        return 0;

    memballoon->info.alias = g_strdup_printf("balloon%d", idx);
    return 0;
}


static int
qemuAssignDeviceTPMAlias(virDomainTPMDef *tpm,
                         int idx)
{
    if (tpm->info.alias)
        return 0;

    tpm->info.alias = g_strdup_printf("tpm%d", idx);
    return 0;
}


int
qemuAssignDeviceRedirdevAlias(virDomainDef *def,
                              virDomainRedirdevDef *redirdev,
                              int idx)
{
    if (redirdev->info.alias)
        return 0;

    if (idx == -1) {
        size_t i;
        idx = 0;
        for (i = 0; i < def->nredirdevs; i++) {
            int thisidx;
            if ((thisidx = qemuDomainDeviceAliasIndex(&def->redirdevs[i]->info, "redir")) < 0)
                continue;
            if (thisidx >= idx)
                idx = thisidx + 1;
        }
    }

    redirdev->info.alias = g_strdup_printf("redir%d", idx);
    return 0;
}


int
qemuAssignDeviceRNGAlias(virDomainDef *def,
                         virDomainRNGDef *rng)
{
    size_t i;
    int maxidx = 0;
    int idx;

    if (rng->info.alias)
        return 0;

    for (i = 0; i < def->nrngs; i++) {
        if ((idx = qemuDomainDeviceAliasIndex(&def->rngs[i]->info, "rng")) >= maxidx)
            maxidx = idx + 1;
    }

    rng->info.alias = g_strdup_printf("rng%d", maxidx);

    return 0;
}


static int
qemuDeviceMemoryGetAliasID(virDomainDef *def,
                           virDomainMemoryDef *mem,
                           bool oldAlias,
                           const char *prefix)
{
    size_t i;
    int maxidx = 0;

    /* virtio-pmem goes onto PCI bus and thus DIMM address is not valid */
    if (!oldAlias && mem->model != VIR_DOMAIN_MEMORY_MODEL_VIRTIO_PMEM)
        return mem->info.addr.dimm.slot;

    for (i = 0; i < def->nmems; i++) {
        int idx;
        if ((idx = qemuDomainDeviceAliasIndex(&def->mems[i]->info, prefix)) >= maxidx)
            maxidx = idx + 1;
    }

    return maxidx;
}


/**
 * qemuAssignDeviceMemoryAlias:
 * @def: domain definition. Necessary only if @oldAlias is true.
 * @mem: memory device definition
 * @oldAlias: Generate the alias according to the order of the device in @def
 *            rather than according to the slot number for legacy reasons.
 *
 * Generates alias for a memory device according to slot number if @oldAlias is
 * false or according to order in @def->mems otherwise.
 *
 * Returns 0 on success, -1 on error.
 */
int
qemuAssignDeviceMemoryAlias(virDomainDef *def,
                            virDomainMemoryDef *mem,
                            bool oldAlias)
{
    const char *prefix = NULL;
    int idx = 0;

    if (mem->info.alias)
        return 0;

    switch (mem->model) {
    case VIR_DOMAIN_MEMORY_MODEL_DIMM:
        prefix = "dimm";
        break;
    case VIR_DOMAIN_MEMORY_MODEL_NVDIMM:
        prefix = "nvdimm";
        break;
    case VIR_DOMAIN_MEMORY_MODEL_VIRTIO_PMEM:
        prefix = "virtiopmem";
        break;
    case VIR_DOMAIN_MEMORY_MODEL_NONE:
    case VIR_DOMAIN_MEMORY_MODEL_LAST:
    default:
        virReportEnumRangeError(virDomainMemoryModel, mem->model);
        return -1;
        break;
    }

    idx = qemuDeviceMemoryGetAliasID(def, mem, oldAlias, prefix);
    mem->info.alias = g_strdup_printf("%s%d", prefix, idx);

    return 0;
}


int
qemuAssignDeviceShmemAlias(virDomainDef *def,
                           virDomainShmemDef *shmem,
                           int idx)
{
    if (shmem->info.alias)
        return 0;

    if (idx == -1) {
        size_t i;
        idx = 0;
        for (i = 0; i < def->nshmems; i++) {
            int thisidx;

            if ((thisidx = qemuDomainDeviceAliasIndex(&def->shmems[i]->info,
                                                      "shmem")) < 0)
                continue;

            if (thisidx >= idx)
                idx = thisidx + 1;
        }
    }

    shmem->info.alias = g_strdup_printf("shmem%d", idx);
    return 0;
}


int
qemuAssignDeviceWatchdogAlias(virDomainWatchdogDef *watchdog)
{
    /* Currently, there's just one watchdog per domain */

    if (watchdog->info.alias)
        return 0;

    watchdog->info.alias = g_strdup("watchdog0");

    return 0;
}

int
qemuAssignDeviceInputAlias(virDomainDef *def,
                           virDomainInputDef *input,
                           int idx)
{
    if (input->info.alias)
        return 0;

    if (idx == -1) {
        int thisidx;
        size_t i;

        for (i = 0; i < def->ninputs; i++) {
            if ((thisidx = qemuDomainDeviceAliasIndex(&def->inputs[i]->info, "input")) >= idx)
                idx = thisidx + 1;
        }
    }

    input->info.alias = g_strdup_printf("input%d", idx);

    return 0;
}


int
qemuAssignDeviceVsockAlias(virDomainVsockDef *vsock)
{
    if (vsock->info.alias)
        return 0;
    vsock->info.alias = g_strdup("vsock0");

    return 0;
}


int
qemuAssignDeviceAliases(virDomainDef *def, virQEMUCaps *qemuCaps)
{
    size_t i;

    for (i = 0; i < def->ndisks; i++) {
        if (qemuAssignDeviceDiskAlias(def, def->disks[i], qemuCaps) < 0)
            return -1;
    }
    for (i = 0; i < def->nnets; i++) {
        if (qemuAssignDeviceNetAlias(def, def->nets[i], -1) < 0)
            return -1;
    }

    for (i = 0; i < def->nfss; i++) {
        if (qemuAssignDeviceFSAlias(def->fss[i], i) < 0)
            return -1;
    }
    for (i = 0; i < def->nsounds; i++) {
        if (qemuAssignDeviceSoundAlias(def->sounds[i], i) < 0)
            return -1;
    }
    for (i = 0; i < def->nhostdevs; i++) {
        /* we can't start assigning at 0, since netdevs may have used
         * up some hostdevN entries already. Also if the HostdevDef is
         * linked to a NetDef, they will share an info and the alias
         * will already be set, so don't try to set it again.
         */
        if (qemuAssignDeviceHostdevAlias(def, &def->hostdevs[i]->info->alias, -1) < 0)
            return -1;
    }
    for (i = 0; i < def->nredirdevs; i++) {
        if (qemuAssignDeviceRedirdevAlias(def, def->redirdevs[i], i) < 0)
            return -1;
    }
    for (i = 0; i < def->nvideos; i++) {
        if (qemuAssignDeviceVideoAlias(def->videos[i], i) < 0)
            return -1;
    }
    for (i = 0; i < def->ncontrollers; i++) {
        if (qemuAssignDeviceControllerAlias(def, qemuCaps, def->controllers[i]) < 0)
            return -1;
    }
    for (i = 0; i < def->ninputs; i++) {
        if (qemuAssignDeviceInputAlias(def, def->inputs[i], i) < 0)
            return -1;
    }
    for (i = 0; i < def->nparallels; i++) {
        if (qemuAssignDeviceChrAlias(def, def->parallels[i], i) < 0)
            return -1;
    }
    for (i = 0; i < def->nserials; i++) {
        if (qemuAssignDeviceChrAlias(def, def->serials[i], i) < 0)
            return -1;
    }
    for (i = 0; i < def->nchannels; i++) {
        if (qemuAssignDeviceChrAlias(def, def->channels[i], i) < 0)
            return -1;
    }
    for (i = 0; i < def->nconsoles; i++) {
        if (qemuAssignDeviceChrAlias(def, def->consoles[i], i) < 0)
            return -1;
    }
    for (i = 0; i < def->nhubs; i++) {
        if (qemuAssignDeviceHubAlias(def->hubs[i], i) < 0)
            return -1;
    }
    for (i = 0; i < def->nshmems; i++) {
        if (qemuAssignDeviceShmemAlias(def, def->shmems[i], i) < 0)
            return -1;
    }
    for (i = 0; i < def->nsmartcards; i++) {
        if (qemuAssignDeviceSmartcardAlias(def->smartcards[i], i) < 0)
            return -1;
    }
    if (def->watchdog) {
        if (qemuAssignDeviceWatchdogAlias(def->watchdog) < 0)
            return -1;
    }
    if (def->memballoon &&
        def->memballoon->model != VIR_DOMAIN_MEMBALLOON_MODEL_NONE) {
        if (qemuAssignDeviceMemballoonAlias(def->memballoon, 0) < 0)
            return -1;
    }
    for (i = 0; i < def->nrngs; i++) {
        if (qemuAssignDeviceRNGAlias(def, def->rngs[i]) < 0)
            return -1;
    }
    for (i = 0; i < def->ntpms; i++) {
        if (qemuAssignDeviceTPMAlias(def->tpms[i], i) < 0)
            return -1;
    }
    for (i = 0; i < def->nmems; i++) {
        if (qemuAssignDeviceMemoryAlias(def, def->mems[i], false) < 0)
            return -1;
    }
    if (def->vsock) {
        if (qemuAssignDeviceVsockAlias(def->vsock) < 0)
            return -1;
    }

    return 0;
}


/* qemuAliasDiskDriveFromDisk
 * @disk: Pointer to a disk definition
 *
 * Generate and return an alias for the device disk '-drive'
 *
 * Returns NULL with error or a string containing the alias
 */
char *
qemuAliasDiskDriveFromDisk(const virDomainDiskDef *disk)
{
    char *ret;

    if (!disk->info.alias) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("disk does not have an alias"));
        return NULL;
    }

    ret = g_strdup_printf("%s%s", QEMU_DRIVE_HOST_PREFIX, disk->info.alias);
    return ret;
}


/* qemuAliasDiskDriveSkipPrefix:
 * @dev_name: Pointer to a const char string
 *
 * If the QEMU_DRIVE_HOST_PREFIX exists in the input string, then
 * increment the pointer and return it
 */
const char *
qemuAliasDiskDriveSkipPrefix(const char *dev_name)
{
    if (STRPREFIX(dev_name, QEMU_DRIVE_HOST_PREFIX))
        dev_name += strlen(QEMU_DRIVE_HOST_PREFIX);
    return dev_name;
}


/* qemuAliasFromHostdev
 * @hostdev: Pointer to host device
 *
 * Generate and return a string containing a drive alias
 */
char *
qemuAliasFromHostdev(const virDomainHostdevDef *hostdev)
{
    char *ret;

    if (!hostdev->info->alias) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("hostdev does not have an alias"));
        return NULL;
    }

    ret = g_strdup_printf("%s-%s",
                          virDomainDeviceAddressTypeToString(hostdev->info->type),
                          hostdev->info->alias);
    return ret;
}


/* qemuDomainGetMasterKeyAlias:
 *
 * Generate and return the masterKey alias
 *
 * Returns NULL or a string containing the master key alias
 */
char *
qemuDomainGetMasterKeyAlias(void)
{
    char *alias;

    alias = g_strdup("masterKey0");

    return alias;
}


/* qemuAliasForSecret:
 * @parentalias: alias of the parent object
 * @obj: optional sub-object of the parent device the secret is for
 *
 * Generate alias for a secret object used by @parentalias device or one of
 * the dependencies of the device described by @obj.
 */
char *
qemuAliasForSecret(const char *parentalias,
                   const char *obj)
{
    if (obj)
        return g_strdup_printf("%s-%s-secret0", parentalias, obj);
    else
        return g_strdup_printf("%s-secret0", parentalias);
}

/* qemuAliasTLSObjFromSrcAlias
 * @srcAlias: Pointer to a source alias string
 *
 * Generate and return a string to be used as the TLS object alias
 */
char *
qemuAliasTLSObjFromSrcAlias(const char *srcAlias)
{
    char *ret;

    ret = g_strdup_printf("obj%s_tls0", srcAlias);

    return ret;
}


/* qemuAliasChardevFromDevAlias:
 * @devAlias: pointer do device alias
 *
 * Generate and return a string to be used as chardev alias.
 */
char *
qemuAliasChardevFromDevAlias(const char *devAlias)
{
    char *ret;

    ret = g_strdup_printf("char%s", devAlias);

    return ret;
}


const char *
qemuDomainGetManagedPRAlias(void)
{
    return "pr-helper0";
}


char *
qemuDomainGetUnmanagedPRAlias(const char *parentalias)
{
    char *ret;

    ret = g_strdup_printf("pr-helper-%s", parentalias);

    return ret;
}


const char *
qemuDomainGetDBusVMStateAlias(void)
{
    return "dbus-vmstate0";
}

char *
qemuDomainGetVhostUserChrAlias(const char *devalias)
{
    return g_strdup_printf("chr-vu-%s", devalias);
}
