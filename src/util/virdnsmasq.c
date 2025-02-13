/*
 * virdnsmasq.c: Helper APIs for managing dnsmasq
 *
 * Copyright (C) 2007-2013 Red Hat, Inc.
 * Copyright (C) 2010 Satoru SATOH <satoru.satoh@gmail.com>
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
 *
 * Based on iptables.c
 */

#include <config.h>

#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <signal.h>

#include "internal.h"
#include "datatypes.h"
#include "virbitmap.h"
#include "virdnsmasq.h"
#include "virutil.h"
#include "vircommand.h"
#include "viralloc.h"
#include "virerror.h"
#include "virlog.h"
#include "virfile.h"
#include "virstring.h"

#define VIR_FROM_THIS VIR_FROM_NETWORK

VIR_LOG_INIT("util.dnsmasq");

#define DNSMASQ_HOSTSFILE_SUFFIX "hostsfile"
#define DNSMASQ_ADDNHOSTSFILE_SUFFIX "addnhosts"

static void
dhcphostFreeContent(dnsmasqDhcpHost *host)
{
    g_free(host->host);
}

static void
addnhostFreeContent(dnsmasqAddnHost *host)
{
    size_t i;

    for (i = 0; i < host->nhostnames; i++)
        g_free(host->hostnames[i]);
    g_free(host->hostnames);
    g_free(host->ip);
}

static void
addnhostsFree(dnsmasqAddnHostsfile *addnhostsfile)
{
    size_t i;

    if (addnhostsfile->hosts) {
        for (i = 0; i < addnhostsfile->nhosts; i++)
            addnhostFreeContent(&addnhostsfile->hosts[i]);

        g_free(addnhostsfile->hosts);

        addnhostsfile->nhosts = 0;
    }

    g_free(addnhostsfile->path);

    g_free(addnhostsfile);
}

static int
addnhostsAdd(dnsmasqAddnHostsfile *addnhostsfile,
             virSocketAddr *ip,
             const char *name)
{
    char *ipstr = NULL;
    int idx = -1;
    size_t i;

    if (!(ipstr = virSocketAddrFormat(ip)))
        return -1;

    for (i = 0; i < addnhostsfile->nhosts; i++) {
        if (STREQ((const char *)addnhostsfile->hosts[i].ip, (const char *)ipstr)) {
            idx = i;
            break;
        }
    }

    if (idx < 0) {
        VIR_REALLOC_N(addnhostsfile->hosts, addnhostsfile->nhosts + 1);

        idx = addnhostsfile->nhosts;
        addnhostsfile->hosts[idx].hostnames = g_new0(char *, 1);

        addnhostsfile->hosts[idx].ip = g_strdup(ipstr);

        addnhostsfile->hosts[idx].nhostnames = 0;
        addnhostsfile->nhosts++;
    }

    VIR_REALLOC_N(addnhostsfile->hosts[idx].hostnames, addnhostsfile->hosts[idx].nhostnames + 1);

    addnhostsfile->hosts[idx].hostnames[addnhostsfile->hosts[idx].nhostnames] = g_strdup(name);

    VIR_FREE(ipstr);

    addnhostsfile->hosts[idx].nhostnames++;

    return 0;
}

static dnsmasqAddnHostsfile *
addnhostsNew(const char *name,
             const char *config_dir)
{
    dnsmasqAddnHostsfile *addnhostsfile;
    g_auto(virBuffer) buf = VIR_BUFFER_INITIALIZER;

    addnhostsfile = g_new0(dnsmasqAddnHostsfile, 1);

    addnhostsfile->hosts = NULL;
    addnhostsfile->nhosts = 0;

    virBufferAsprintf(&buf, "%s", config_dir);
    virBufferEscapeString(&buf, "/%s", name);
    virBufferAsprintf(&buf, ".%s", DNSMASQ_ADDNHOSTSFILE_SUFFIX);

    if (!(addnhostsfile->path = virBufferContentAndReset(&buf)))
        goto error;

    return addnhostsfile;

 error:
    addnhostsFree(addnhostsfile);
    return NULL;
}

static int
addnhostsWrite(const char *path,
               dnsmasqAddnHost *hosts,
               unsigned int nhosts)
{
    g_autofree char *tmp = NULL;
    FILE *f;
    bool istmp = true;
    size_t i, j;
    int rc = 0;

    /* even if there are 0 hosts, create a 0 length file, to allow
     * for runtime addition.
     */

    tmp = g_strdup_printf("%s.new", path);

    if (!(f = fopen(tmp, "w"))) {
        istmp = false;
        if (!(f = fopen(path, "w"))) {
            rc = -errno;
            goto cleanup;
        }
    }

    for (i = 0; i < nhosts; i++) {
        if (fputs(hosts[i].ip, f) == EOF || fputc('\t', f) == EOF) {
            rc = -errno;
            VIR_FORCE_FCLOSE(f);

            if (istmp)
                unlink(tmp);

            goto cleanup;
        }

        for (j = 0; j < hosts[i].nhostnames; j++) {
            if (fputs(hosts[i].hostnames[j], f) == EOF || fputc('\t', f) == EOF) {
                rc = -errno;
                VIR_FORCE_FCLOSE(f);

                if (istmp)
                    unlink(tmp);

                goto cleanup;
            }
        }

        if (fputc('\n', f) == EOF) {
            rc = -errno;
            VIR_FORCE_FCLOSE(f);

            if (istmp)
                unlink(tmp);

            goto cleanup;
        }
    }

    if (VIR_FCLOSE(f) == EOF) {
        rc = -errno;
        goto cleanup;
    }

    if (istmp && rename(tmp, path) < 0) {
        rc = -errno;
        unlink(tmp);
        goto cleanup;
    }

 cleanup:
    return rc;
}

static int
addnhostsSave(dnsmasqAddnHostsfile *addnhostsfile)
{
    int err = addnhostsWrite(addnhostsfile->path, addnhostsfile->hosts,
                             addnhostsfile->nhosts);

    if (err < 0) {
        virReportSystemError(-err, _("cannot write config file '%s'"),
                             addnhostsfile->path);
        return -1;
    }

    return 0;
}

static int
genericFileDelete(char *path)
{
    if (!virFileExists(path))
        return 0;

    if (unlink(path) < 0) {
        virReportSystemError(errno, _("cannot remove config file '%s'"),
                             path);
        return -1;
    }

    return 0;
}

static void
hostsfileFree(dnsmasqHostsfile *hostsfile)
{
    size_t i;

    if (hostsfile->hosts) {
        for (i = 0; i < hostsfile->nhosts; i++)
            dhcphostFreeContent(&hostsfile->hosts[i]);

        g_free(hostsfile->hosts);

        hostsfile->nhosts = 0;
    }

    g_free(hostsfile->path);

    g_free(hostsfile);
}

/* Note:  There are many additional dhcp-host specifications
 * supported by dnsmasq.  There are only the basic ones.
 */
static int
hostsfileAdd(dnsmasqHostsfile *hostsfile,
             const char *mac,
             virSocketAddr *ip,
             const char *name,
             const char *id,
             const char *leasetime,
             bool ipv6)
{
    g_autofree char *ipstr = NULL;
    g_auto(virBuffer) buf = VIR_BUFFER_INITIALIZER;

    VIR_REALLOC_N(hostsfile->hosts, hostsfile->nhosts + 1);

    if (!(ipstr = virSocketAddrFormat(ip)))
        return -1;

    /* the first test determines if it is a dhcpv6 host */
    if (ipv6) {
        if (name && id) {
            virBufferAsprintf(&buf, "id:%s,%s", id, name);
        } else if (name && !id) {
            virBufferAsprintf(&buf, "%s", name);
        } else if (!name && id) {
            virBufferAsprintf(&buf, "id:%s", id);
        }
        virBufferAsprintf(&buf, ",[%s]", ipstr);
    } else if (name && mac) {
        virBufferAsprintf(&buf, "%s,%s,%s", mac, ipstr, name);
    } else if (name && !mac) {
        virBufferAsprintf(&buf, "%s,%s", name, ipstr);
    } else {
        virBufferAsprintf(&buf, "%s,%s", mac, ipstr);
    }

    if (leasetime)
        virBufferAsprintf(&buf, ",%s", leasetime);

    if (!(hostsfile->hosts[hostsfile->nhosts].host = virBufferContentAndReset(&buf)))
        return -1;

    hostsfile->nhosts++;

    return 0;
}

static dnsmasqHostsfile *
hostsfileNew(const char *name,
             const char *config_dir)
{
    dnsmasqHostsfile *hostsfile;
    g_auto(virBuffer) buf = VIR_BUFFER_INITIALIZER;

    hostsfile = g_new0(dnsmasqHostsfile, 1);

    hostsfile->hosts = NULL;
    hostsfile->nhosts = 0;

    virBufferAsprintf(&buf, "%s", config_dir);
    virBufferEscapeString(&buf, "/%s", name);
    virBufferAsprintf(&buf, ".%s", DNSMASQ_HOSTSFILE_SUFFIX);

    if (!(hostsfile->path = virBufferContentAndReset(&buf)))
        goto error;
    return hostsfile;

 error:
    hostsfileFree(hostsfile);
    return NULL;
}

static int
hostsfileWrite(const char *path,
               dnsmasqDhcpHost *hosts,
               unsigned int nhosts)
{
    g_autofree char *tmp = NULL;
    FILE *f;
    bool istmp = true;
    size_t i;
    int rc = 0;

    /* even if there are 0 hosts, create a 0 length file, to allow
     * for runtime addition.
     */

    tmp = g_strdup_printf("%s.new", path);

    if (!(f = fopen(tmp, "w"))) {
        istmp = false;
        if (!(f = fopen(path, "w"))) {
            rc = -errno;
            goto cleanup;
        }
    }

    for (i = 0; i < nhosts; i++) {
        if (fputs(hosts[i].host, f) == EOF || fputc('\n', f) == EOF) {
            rc = -errno;
            VIR_FORCE_FCLOSE(f);

            if (istmp)
                unlink(tmp);

            goto cleanup;
        }
    }

    if (VIR_FCLOSE(f) == EOF) {
        rc = -errno;
        goto cleanup;
    }

    if (istmp && rename(tmp, path) < 0) {
        rc = -errno;
        unlink(tmp);
        goto cleanup;
    }

 cleanup:
    return rc;
}

static int
hostsfileSave(dnsmasqHostsfile *hostsfile)
{
    int err = hostsfileWrite(hostsfile->path, hostsfile->hosts,
                             hostsfile->nhosts);

    if (err < 0) {
        virReportSystemError(-err, _("cannot write config file '%s'"),
                             hostsfile->path);
        return -1;
    }

    return 0;
}

/**
 * dnsmasqContextNew:
 *
 * Create a new Dnsmasq context
 *
 * Returns a pointer to the new structure or NULL in case of error
 */
dnsmasqContext *
dnsmasqContextNew(const char *network_name,
                  const char *config_dir)
{
    dnsmasqContext *ctx;

    ctx = g_new0(dnsmasqContext, 1);

    ctx->config_dir = g_strdup(config_dir);

    if (!(ctx->hostsfile = hostsfileNew(network_name, config_dir)))
        goto error;
    if (!(ctx->addnhostsfile = addnhostsNew(network_name, config_dir)))
        goto error;

    return ctx;

 error:
    dnsmasqContextFree(ctx);
    return NULL;
}

/**
 * dnsmasqContextFree:
 * @ctx: pointer to the dnsmasq context
 *
 * Free the resources associated with a dnsmasq context
 */
void
dnsmasqContextFree(dnsmasqContext *ctx)
{
    if (!ctx)
        return;

    g_free(ctx->config_dir);

    if (ctx->hostsfile)
        hostsfileFree(ctx->hostsfile);
    if (ctx->addnhostsfile)
        addnhostsFree(ctx->addnhostsfile);

    g_free(ctx);
}

/**
 * dnsmasqAddDhcpHost:
 * @ctx: pointer to the dnsmasq context for each network
 * @mac: pointer to the string contains mac address of the host
 * @ip: pointer to the socket address contains ip of the host
 * @name: pointer to the string contains hostname of the host or NULL
 *
 * Add dhcp-host entry.
 */
int
dnsmasqAddDhcpHost(dnsmasqContext *ctx,
                   const char *mac,
                   virSocketAddr *ip,
                   const char *name,
                   const char *id,
                   const char *leasetime,
                   bool ipv6)
{
    return hostsfileAdd(ctx->hostsfile, mac, ip, name, id, leasetime, ipv6);
}

/*
 * dnsmasqAddHost:
 * @ctx: pointer to the dnsmasq context for each network
 * @ip: pointer to the socket address contains ip of the host
 * @name: pointer to the string contains hostname of the host
 *
 * Add additional host entry.
 */

int
dnsmasqAddHost(dnsmasqContext *ctx,
               virSocketAddr *ip,
               const char *name)
{
    return addnhostsAdd(ctx->addnhostsfile, ip, name);
}

/**
 * dnsmasqSave:
 * @ctx: pointer to the dnsmasq context for each network
 *
 * Saves all the configurations associated with a context to disk.
 */
int
dnsmasqSave(const dnsmasqContext *ctx)
{
    int ret = 0;

    if (g_mkdir_with_parents(ctx->config_dir, 0777) < 0) {
        virReportSystemError(errno, _("cannot create config directory '%s'"),
                             ctx->config_dir);
        return -1;
    }

    if (ctx->hostsfile)
        ret = hostsfileSave(ctx->hostsfile);
    if (ret == 0) {
        if (ctx->addnhostsfile)
            ret = addnhostsSave(ctx->addnhostsfile);
    }

    return ret;
}


/**
 * dnsmasqDelete:
 * @ctx: pointer to the dnsmasq context for each network
 *
 * Delete all the configuration files associated with a context.
 */
int
dnsmasqDelete(const dnsmasqContext *ctx)
{
    int ret = 0;

    if (ctx->hostsfile)
        ret = genericFileDelete(ctx->hostsfile->path);
    if (ctx->addnhostsfile)
        ret = genericFileDelete(ctx->addnhostsfile->path);

    return ret;
}

/**
 * dnsmasqReload:
 * @pid: the pid of the target dnsmasq process
 *
 * Reloads all the configurations associated to a context
 */
int
dnsmasqReload(pid_t pid G_GNUC_UNUSED)
{
#ifndef WIN32
    if (kill(pid, SIGHUP) != 0) {
        virReportSystemError(errno,
                             _("Failed to make dnsmasq (PID: %d)"
                               " reload config files."),
                             pid);
        return -1;
    }
#endif /* WIN32 */

    return 0;
}

/*
 * dnsmasqCapabilities functions - provide useful information about the
 * version of dnsmasq on this machine.
 *
 */
struct _dnsmasqCaps {
    virObject parent;
    char *binaryPath;
    bool noRefresh;
    time_t mtime;
    virBitmap *flags;
    unsigned long version;
};

static virClass *dnsmasqCapsClass;

static void
dnsmasqCapsDispose(void *obj)
{
    dnsmasqCaps *caps = obj;

    virBitmapFree(caps->flags);
    g_free(caps->binaryPath);
}

static int dnsmasqCapsOnceInit(void)
{
    if (!VIR_CLASS_NEW(dnsmasqCaps, virClassForObject()))
        return -1;

    return 0;
}

VIR_ONCE_GLOBAL_INIT(dnsmasqCaps);

static void
dnsmasqCapsSet(dnsmasqCaps *caps,
               dnsmasqCapsFlags flag)
{
    ignore_value(virBitmapSetBit(caps->flags, flag));
}


#define DNSMASQ_VERSION_STR "Dnsmasq version "

static int
dnsmasqCapsSetFromBuffer(dnsmasqCaps *caps, const char *buf)
{
    int len;
    const char *p;

    caps->noRefresh = true;

    p = STRSKIP(buf, DNSMASQ_VERSION_STR);
    if (!p)
       goto fail;

    virSkipToDigit(&p);

    if (virParseVersionString(p, &caps->version, true) < 0)
        goto fail;

    if (strstr(buf, "--bind-dynamic"))
        dnsmasqCapsSet(caps, DNSMASQ_CAPS_BIND_DYNAMIC);

    /* if this string is a part of the --version output, dnsmasq
     * has been patched to use SO_BINDTODEVICE when listening,
     * so that it will only accept requests that arrived on the
     * listening interface(s)
     */
    if (strstr(buf, "--bind-interfaces with SO_BINDTODEVICE"))
        dnsmasqCapsSet(caps, DNSMASQ_CAPS_BINDTODEVICE);

    if (strstr(buf, "--ra-param"))
        dnsmasqCapsSet(caps, DNSMASQ_CAPS_RA_PARAM);

    VIR_INFO("dnsmasq version is %d.%d, --bind-dynamic is %spresent, "
             "SO_BINDTODEVICE is %sin use, --ra-param is %spresent",
             (int)caps->version / 1000000,
             (int)(caps->version % 1000000) / 1000,
             dnsmasqCapsGet(caps, DNSMASQ_CAPS_BIND_DYNAMIC) ? "" : "NOT ",
             dnsmasqCapsGet(caps, DNSMASQ_CAPS_BINDTODEVICE) ? "" : "NOT ",
             dnsmasqCapsGet(caps, DNSMASQ_CAPS_RA_PARAM) ? "" : "NOT ");
    return 0;

 fail:
    p = strchr(buf, '\n');
    if (!p)
        len = strlen(buf);
    else
        len = p - buf;
    virReportError(VIR_ERR_INTERNAL_ERROR,
                   _("cannot parse %s version number in '%.*s'"),
                   caps->binaryPath, len, buf);
    return -1;

}

static int
dnsmasqCapsSetFromFile(dnsmasqCaps *caps, const char *path)
{
    int ret = -1;
    g_autofree char *buf = NULL;

    if (virFileReadAll(path, 1024 * 1024, &buf) < 0)
        goto cleanup;

    ret = dnsmasqCapsSetFromBuffer(caps, buf);

 cleanup:
    return ret;
}

static int
dnsmasqCapsRefreshInternal(dnsmasqCaps *caps, bool force)
{
    int ret = -1;
    struct stat sb;
    virCommand *cmd = NULL;
    g_autofree char *help = NULL;
    g_autofree char *version = NULL;
    g_autofree char *complete = NULL;

    if (!caps || caps->noRefresh)
        return 0;

    if (stat(caps->binaryPath, &sb) < 0) {
        virReportSystemError(errno, _("Cannot check dnsmasq binary %s"),
                             caps->binaryPath);
        return -1;
    }
    if (!force && caps->mtime == sb.st_mtime)
        return 0;
    caps->mtime = sb.st_mtime;

    /* Make sure the binary we are about to try exec'ing exists.
     * Technically we could catch the exec() failure, but that's
     * in a sub-process so it's hard to feed back a useful error.
     */
    if (!virFileIsExecutable(caps->binaryPath)) {
        virReportSystemError(errno, _("dnsmasq binary %s is not executable"),
                             caps->binaryPath);
        goto cleanup;
    }

    cmd = virCommandNewArgList(caps->binaryPath, "--version", NULL);
    virCommandSetOutputBuffer(cmd, &version);
    virCommandAddEnvPassCommon(cmd);
    virCommandClearCaps(cmd);
    if (virCommandRun(cmd, NULL) < 0)
        goto cleanup;
    virCommandFree(cmd);

    cmd = virCommandNewArgList(caps->binaryPath, "--help", NULL);
    virCommandSetOutputBuffer(cmd, &help);
    virCommandAddEnvPassCommon(cmd);
    virCommandClearCaps(cmd);
    if (virCommandRun(cmd, NULL) < 0)
        goto cleanup;

    complete = g_strdup_printf("%s\n%s", version, help);

    ret = dnsmasqCapsSetFromBuffer(caps, complete);

 cleanup:
    virCommandFree(cmd);
    return ret;
}

static dnsmasqCaps *
dnsmasqCapsNewEmpty(const char *binaryPath)
{
    dnsmasqCaps *caps;

    if (dnsmasqCapsInitialize() < 0)
        return NULL;
    if (!(caps = virObjectNew(dnsmasqCapsClass)))
        return NULL;
    caps->flags = virBitmapNew(DNSMASQ_CAPS_LAST);
    caps->binaryPath = g_strdup(binaryPath ? binaryPath : DNSMASQ);
    return caps;
}

dnsmasqCaps *
dnsmasqCapsNewFromBuffer(const char *buf, const char *binaryPath)
{
    dnsmasqCaps *caps = dnsmasqCapsNewEmpty(binaryPath);

    if (!caps)
        return NULL;

    if (dnsmasqCapsSetFromBuffer(caps, buf) < 0) {
        virObjectUnref(caps);
        return NULL;
    }
    return caps;
}

dnsmasqCaps *
dnsmasqCapsNewFromFile(const char *dataPath, const char *binaryPath)
{
    dnsmasqCaps *caps = dnsmasqCapsNewEmpty(binaryPath);

    if (!caps)
        return NULL;

    if (dnsmasqCapsSetFromFile(caps, dataPath) < 0) {
        virObjectUnref(caps);
        return NULL;
    }
    return caps;
}

dnsmasqCaps *
dnsmasqCapsNewFromBinary(const char *binaryPath)
{
    dnsmasqCaps *caps = dnsmasqCapsNewEmpty(binaryPath);

    if (!caps)
        return NULL;

    if (dnsmasqCapsRefreshInternal(caps, true) < 0) {
        virObjectUnref(caps);
        return NULL;
    }
    return caps;
}

/** dnsmasqCapsRefresh:
 *
 *   Refresh an existing caps object if the binary has changed. If
 *   there isn't yet a caps object (if it's NULL), create a new one.
 *
 *   Returns 0 on success, -1 on failure
 */
int
dnsmasqCapsRefresh(dnsmasqCaps **caps, const char *binaryPath)
{
    if (!*caps) {
        *caps = dnsmasqCapsNewFromBinary(binaryPath);
        return *caps ? 0 : -1;
    }
    return dnsmasqCapsRefreshInternal(*caps, false);
}

const char *
dnsmasqCapsGetBinaryPath(dnsmasqCaps *caps)
{
    return caps ? caps->binaryPath : DNSMASQ;
}

unsigned long
dnsmasqCapsGetVersion(dnsmasqCaps *caps)
{
    if (caps)
        return caps->version;
    else
        return 0;
}

bool
dnsmasqCapsGet(dnsmasqCaps *caps, dnsmasqCapsFlags flag)
{
    return caps && virBitmapIsBitSet(caps->flags, flag);
}


/** dnsmasqDhcpHostsToString:
 *
 *   Turns a vector of dnsmasqDhcpHost into the string that is ought to be
 *   stored in the hostsfile, this functionality is split to make hostsfiles
 *   testable. Returns NULL if nhosts is 0.
 */
char *
dnsmasqDhcpHostsToString(dnsmasqDhcpHost *hosts,
                         unsigned int nhosts)
{
    size_t i;
    g_auto(virBuffer) buf = VIR_BUFFER_INITIALIZER;

    for (i = 0; i < nhosts; i++)
        virBufferAsprintf(&buf, "%s\n", hosts[i].host);

    return virBufferContentAndReset(&buf);
}
