/*
 * virxml.c: helper APIs for dealing with XML documents
 *
 * Copyright (C) 2005, 2007-2012 Red Hat, Inc.
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

#include <stdarg.h>
#include <math.h>               /* for isnan() */
#include <sys/stat.h>

#include <libxml/xpathInternals.h>

#include "virerror.h"
#include "virxml.h"
#include "virbuffer.h"
#include "viralloc.h"
#include "virfile.h"
#include "virstring.h"
#include "virutil.h"

#define VIR_FROM_THIS VIR_FROM_XML

#define virGenericReportError(from, code, ...) \
        virReportErrorHelper(from, code, __FILE__, \
                             __FUNCTION__, __LINE__, __VA_ARGS__)

/* Internal data to be passed to SAX parser and used by error handler. */
struct virParserData {
    int domcode;
};


xmlXPathContextPtr
virXMLXPathContextNew(xmlDocPtr xml)
{
    xmlXPathContextPtr ctxt;

    if (!(ctxt = xmlXPathNewContext(xml)))
        abort();

    return ctxt;
}


/**
 * virXPathString:
 * @xpath: the XPath string to evaluate
 * @ctxt: an XPath context
 *
 * Convenience function to evaluate an XPath string
 *
 * Returns a new string which must be deallocated by the caller or NULL
 *         if the evaluation failed.
 */
char *
virXPathString(const char *xpath,
               xmlXPathContextPtr ctxt)
{
    xmlXPathObjectPtr obj;
    char *ret;

    if ((ctxt == NULL) || (xpath == NULL)) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s", _("Invalid parameter to virXPathString()"));
        return NULL;
    }
    obj = xmlXPathEval(BAD_CAST xpath, ctxt);
    if ((obj == NULL) || (obj->type != XPATH_STRING) ||
        (obj->stringval == NULL) || (obj->stringval[0] == 0)) {
        xmlXPathFreeObject(obj);
        return NULL;
    }
    ret = g_strdup((char *)obj->stringval);
    xmlXPathFreeObject(obj);
    return ret;
}


static char *
virXMLStringLimitInternal(char *value,
                          size_t maxlen,
                          const char *name)
{
    if (value != NULL && strlen(value) >= maxlen) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("'%s' value longer than '%zu' bytes"),
                       name, maxlen);
        VIR_FREE(value);
        return NULL;
    }

    return value;
}


/**
 * virXPathStringLimit:
 * @xpath: the XPath string to evaluate
 * @maxlen: maximum length permitted string
 * @ctxt: an XPath context
 *
 * Wrapper for virXPathString, which validates the length of the returned
 * string.
 *
 * Returns a new string which must be deallocated by the caller or NULL if
 * the evaluation failed.
 */
char *
virXPathStringLimit(const char *xpath,
                    size_t maxlen,
                    xmlXPathContextPtr ctxt)
{
    char *tmp = virXPathString(xpath, ctxt);

    return virXMLStringLimitInternal(tmp, maxlen, xpath);
}

/**
 * virXPathNumber:
 * @xpath: the XPath string to evaluate
 * @ctxt: an XPath context
 * @value: the returned double value
 *
 * Convenience function to evaluate an XPath number
 *
 * Returns 0 in case of success in which case @value is set,
 *         or -1 if the evaluation failed.
 */
int
virXPathNumber(const char *xpath,
               xmlXPathContextPtr ctxt,
               double *value)
{
    xmlXPathObjectPtr obj;

    if ((ctxt == NULL) || (xpath == NULL) || (value == NULL)) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s", _("Invalid parameter to virXPathNumber()"));
        return -1;
    }
    obj = xmlXPathEval(BAD_CAST xpath, ctxt);
    if ((obj == NULL) || (obj->type != XPATH_NUMBER) ||
        (isnan(obj->floatval))) {
        xmlXPathFreeObject(obj);
        return -1;
    }

    *value = obj->floatval;
    xmlXPathFreeObject(obj);
    return 0;
}

static int
virXPathLongBase(const char *xpath,
                 xmlXPathContextPtr ctxt,
                 int base,
                 long *value)
{
    xmlXPathObjectPtr obj;
    int ret = 0;

    if ((ctxt == NULL) || (xpath == NULL) || (value == NULL)) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s", _("Invalid parameter to virXPathLong()"));
        return -1;
    }
    obj = xmlXPathEval(BAD_CAST xpath, ctxt);
    if ((obj != NULL) && (obj->type == XPATH_STRING) &&
        (obj->stringval != NULL) && (obj->stringval[0] != 0)) {
        if (virStrToLong_l((char *) obj->stringval, NULL, base, value) < 0)
            ret = -2;
    } else if ((obj != NULL) && (obj->type == XPATH_NUMBER) &&
               (!(isnan(obj->floatval)))) {
        *value = (long) obj->floatval;
        if (*value != obj->floatval)
            ret = -2;
    } else {
        ret = -1;
    }

    xmlXPathFreeObject(obj);
    return ret;
}

/**
 * virXPathInt:
 * @xpath: the XPath string to evaluate
 * @ctxt: an XPath context
 * @value: the returned int value
 *
 * Convenience function to evaluate an XPath number
 *
 * Returns 0 in case of success in which case @value is set,
 *         or -1 if the XPath evaluation failed or -2 if the
 *         value doesn't have an int format.
 */
int
virXPathInt(const char *xpath,
            xmlXPathContextPtr ctxt,
            int *value)
{
    long tmp;
    int ret;

    ret = virXPathLongBase(xpath, ctxt, 10, &tmp);
    if (ret < 0)
        return ret;
    if ((int) tmp != tmp)
        return -2;
    *value = tmp;
    return 0;
}

/**
 * virXPathLong:
 * @xpath: the XPath string to evaluate
 * @ctxt: an XPath context
 * @value: the returned long value
 *
 * Convenience function to evaluate an XPath number
 *
 * Returns 0 in case of success in which case @value is set,
 *         or -1 if the XPath evaluation failed or -2 if the
 *         value doesn't have a long format.
 */
int
virXPathLong(const char *xpath,
             xmlXPathContextPtr ctxt,
             long *value)
{
    return virXPathLongBase(xpath, ctxt, 10, value);
}

/**
 * virXPathLongHex:
 * @xpath: the XPath string to evaluate
 * @ctxt: an XPath context
 * @value: the returned long value
 *
 * Convenience function to evaluate an XPath number
 * according to a base of 16
 *
 * Returns 0 in case of success in which case @value is set,
 *         or -1 if the XPath evaluation failed or -2 if the
 *         value doesn't have a long format.
 */
int
virXPathLongHex(const char *xpath,
                xmlXPathContextPtr ctxt,
                long *value)
{
    return virXPathLongBase(xpath, ctxt, 16, value);
}

static int
virXPathULongBase(const char *xpath,
                  xmlXPathContextPtr ctxt,
                  int base,
                  unsigned long *value)
{
    xmlXPathObjectPtr obj;
    int ret = 0;

    if ((ctxt == NULL) || (xpath == NULL) || (value == NULL)) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s", _("Invalid parameter to virXPathULong()"));
        return -1;
    }
    obj = xmlXPathEval(BAD_CAST xpath, ctxt);
    if ((obj != NULL) && (obj->type == XPATH_STRING) &&
        (obj->stringval != NULL) && (obj->stringval[0] != 0)) {
        if (virStrToLong_ul((char *) obj->stringval, NULL, base, value) < 0)
            ret = -2;
    } else if ((obj != NULL) && (obj->type == XPATH_NUMBER) &&
               (!(isnan(obj->floatval)))) {
        *value = (unsigned long) obj->floatval;
        if (*value != obj->floatval)
            ret = -2;
    } else {
        ret = -1;
    }

    xmlXPathFreeObject(obj);
    return ret;
}

/**
 * virXPathUInt:
 * @xpath: the XPath string to evaluate
 * @ctxt: an XPath context
 * @value: the returned int value
 *
 * Convenience function to evaluate an XPath number
 *
 * Returns 0 in case of success in which case @value is set,
 *         or -1 if the XPath evaluation failed or -2 if the
 *         value doesn't have an int format.
 */
int
virXPathUInt(const char *xpath,
             xmlXPathContextPtr ctxt,
             unsigned int *value)
{
    unsigned long tmp;
    int ret;

    ret = virXPathULongBase(xpath, ctxt, 10, &tmp);
    if (ret < 0)
        return ret;
    if ((unsigned int) tmp != tmp)
        return -2;
    *value = tmp;
    return 0;
}

/**
 * virXPathULong:
 * @xpath: the XPath string to evaluate
 * @ctxt: an XPath context
 * @value: the returned long value
 *
 * Convenience function to evaluate an XPath number
 *
 * Returns 0 in case of success in which case @value is set,
 *         or -1 if the XPath evaluation failed or -2 if the
 *         value doesn't have a long format.
 */
int
virXPathULong(const char *xpath,
              xmlXPathContextPtr ctxt,
              unsigned long *value)
{
    return virXPathULongBase(xpath, ctxt, 10, value);
}

/**
 * virXPathUHex:
 * @xpath: the XPath string to evaluate
 * @ctxt: an XPath context
 * @value: the returned long value
 *
 * Convenience function to evaluate an XPath number
 * according to base of 16
 *
 * Returns 0 in case of success in which case @value is set,
 *         or -1 if the XPath evaluation failed or -2 if the
 *         value doesn't have a long format.
 */
int
virXPathULongHex(const char *xpath,
                 xmlXPathContextPtr ctxt,
                 unsigned long *value)
{
    return virXPathULongBase(xpath, ctxt, 16, value);
}

/**
 * virXPathULongLong:
 * @xpath: the XPath string to evaluate
 * @ctxt: an XPath context
 * @value: the returned long long value
 *
 * Convenience function to evaluate an XPath number
 *
 * Returns 0 in case of success in which case @value is set,
 *         or -1 if the XPath evaluation failed or -2 if the
 *         value doesn't have a long format.
 */
int
virXPathULongLong(const char *xpath,
                  xmlXPathContextPtr ctxt,
                  unsigned long long *value)
{
    xmlXPathObjectPtr obj;
    int ret = 0;

    if ((ctxt == NULL) || (xpath == NULL) || (value == NULL)) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s", _("Invalid parameter to virXPathULong()"));
        return -1;
    }
    obj = xmlXPathEval(BAD_CAST xpath, ctxt);
    if ((obj != NULL) && (obj->type == XPATH_STRING) &&
        (obj->stringval != NULL) && (obj->stringval[0] != 0)) {
        if (virStrToLong_ull((char *) obj->stringval, NULL, 10, value) < 0)
            ret = -2;
    } else if ((obj != NULL) && (obj->type == XPATH_NUMBER) &&
               (!(isnan(obj->floatval)))) {
        *value = (unsigned long long) obj->floatval;
        if (*value != obj->floatval)
            ret = -2;
    } else {
        ret = -1;
    }

    xmlXPathFreeObject(obj);
    return ret;
}

/**
 * virXPathLongLong:
 * @xpath: the XPath string to evaluate
 * @ctxt: an XPath context
 * @value: the returned long long value
 *
 * Convenience function to evaluate an XPath number
 *
 * Returns 0 in case of success in which case @value is set,
 *         or -1 if the XPath evaluation failed or -2 if the
 *         value doesn't have a long format.
 */
int
virXPathLongLong(const char *xpath,
                 xmlXPathContextPtr ctxt,
                 long long *value)
{
    xmlXPathObjectPtr obj;
    int ret = 0;

    if ((ctxt == NULL) || (xpath == NULL) || (value == NULL)) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s", _("Invalid parameter to virXPathLongLong()"));
        return -1;
    }
    obj = xmlXPathEval(BAD_CAST xpath, ctxt);
    if ((obj != NULL) && (obj->type == XPATH_STRING) &&
        (obj->stringval != NULL) && (obj->stringval[0] != 0)) {
        if (virStrToLong_ll((char *) obj->stringval, NULL, 10, value) < 0)
            ret = -2;
    } else if ((obj != NULL) && (obj->type == XPATH_NUMBER) &&
               (!(isnan(obj->floatval)))) {
        *value = (long long) obj->floatval;
        if (*value != obj->floatval)
            ret = -2;
    } else {
        ret = -1;
    }

    xmlXPathFreeObject(obj);
    return ret;
}


/**
 * virXMLCheckIllegalChars:
 * @nodeName: Name of checked node
 * @str: string to check
 * @illegal: illegal chars to check
 *
 * If string contains any of illegal chars VIR_ERR_XML_DETAIL error will be
 * reported.
 *
 * Returns: 0 if string don't contains any of given characters, -1 otherwise
 */
int
virXMLCheckIllegalChars(const char *nodeName,
                        const char *str,
                        const char *illegal)
{
    char *c;
    if ((c = strpbrk(str, illegal))) {
        virReportError(VIR_ERR_XML_DETAIL,
                       _("invalid char in %s: %c"), nodeName, *c);
        return -1;
    }
    return 0;
}


/**
 * virXMLPropString:
 * @node: XML dom node pointer
 * @name: Name of the property (attribute) to get
 *
 * Convenience function to return copy of an attribute value of a XML node.
 *
 * Returns the property (attribute) value as string or NULL in case of failure.
 * The caller is responsible for freeing the returned buffer.
 */
char *
virXMLPropString(xmlNodePtr node,
                 const char *name)
{
    return (char *)xmlGetProp(node, BAD_CAST name);
}


/**
 * virXMLPropStringLimit:
 * @node: XML dom node pointer
 * @name: Name of the property (attribute) to get
 * @maxlen: maximum permitted length of the string
 *
 * Wrapper for virXMLPropString, which validates the length of the returned
 * string.
 *
 * Returns a new string which must be deallocated by the caller or NULL if
 * the evaluation failed.
 */
char *
virXMLPropStringLimit(xmlNodePtr node,
                      const char *name,
                      size_t maxlen)
{
    char *tmp = (char *)xmlGetProp(node, BAD_CAST name);

    return virXMLStringLimitInternal(tmp, maxlen, name);
}


/**
 * virXMLNodeContentString:
 * @node: XML dom node pointer
 *
 * Convenience function to return copy of content of an XML node.
 *
 * Returns the content value as string or NULL in case of failure.
 * The caller is responsible for freeing the returned buffer.
 */
char *
virXMLNodeContentString(xmlNodePtr node)
{
    char *ret = NULL;

    if (node->type !=  XML_ELEMENT_NODE) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("node '%s' has unexpected type %d"),
                       node->name, node->type);
        return NULL;
    }

    ret = (char *)xmlNodeGetContent(node);

    if (!ret) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("node '%s' has unexpected NULL content. This could be caused by malformed input, or a memory allocation failure"),
                       node->name);
        return NULL;
    }

    return ret;
}


/**
 * virXPathBoolean:
 * @xpath: the XPath string to evaluate
 * @ctxt: an XPath context
 *
 * Convenience function to evaluate an XPath boolean
 *
 * Returns 0 if false, 1 if true, or -1 if the evaluation failed.
 */
int
virXPathBoolean(const char *xpath,
                xmlXPathContextPtr ctxt)
{
    xmlXPathObjectPtr obj;
    int ret;

    if ((ctxt == NULL) || (xpath == NULL)) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s", _("Invalid parameter to virXPathBoolean()"));
        return -1;
    }
    obj = xmlXPathEval(BAD_CAST xpath, ctxt);
    if ((obj == NULL) || (obj->type != XPATH_BOOLEAN) ||
        (obj->boolval < 0) || (obj->boolval > 1)) {
        xmlXPathFreeObject(obj);
        return -1;
    }
    ret = obj->boolval;

    xmlXPathFreeObject(obj);
    return ret;
}

/**
 * virXPathNode:
 * @xpath: the XPath string to evaluate
 * @ctxt: an XPath context
 *
 * Convenience function to evaluate an XPath node set and returning
 * only one node, the first one in the set if any
 *
 * Returns a pointer to the node or NULL if the evaluation failed.
 */
xmlNodePtr
virXPathNode(const char *xpath,
             xmlXPathContextPtr ctxt)
{
    xmlXPathObjectPtr obj;
    xmlNodePtr ret;

    if ((ctxt == NULL) || (xpath == NULL)) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s", _("Invalid parameter to virXPathNode()"));
        return NULL;
    }
    obj = xmlXPathEval(BAD_CAST xpath, ctxt);
    if ((obj == NULL) || (obj->type != XPATH_NODESET) ||
        (obj->nodesetval == NULL) || (obj->nodesetval->nodeNr <= 0) ||
        (obj->nodesetval->nodeTab == NULL)) {
        xmlXPathFreeObject(obj);
        return NULL;
    }

    ret = obj->nodesetval->nodeTab[0];
    xmlXPathFreeObject(obj);
    return ret;
}

/**
 * virXPathNodeSet:
 * @xpath: the XPath string to evaluate
 * @ctxt: an XPath context
 * @list: the returned list of nodes (or NULL if only count matters)
 *
 * Convenience function to evaluate an XPath node set
 *
 * Returns the number of nodes found in which case @list is set (and
 *         must be freed) or -1 if the evaluation failed.
 */
int
virXPathNodeSet(const char *xpath,
                xmlXPathContextPtr ctxt,
                xmlNodePtr **list)
{
    xmlXPathObjectPtr obj;
    int ret;

    if ((ctxt == NULL) || (xpath == NULL)) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s", _("Invalid parameter to virXPathNodeSet()"));
        return -1;
    }

    if (list != NULL)
        *list = NULL;

    obj = xmlXPathEval(BAD_CAST xpath, ctxt);
    if (obj == NULL)
        return 0;

    if (obj->type != XPATH_NODESET) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Incorrect xpath '%s'"), xpath);
        xmlXPathFreeObject(obj);
        return -1;
    }

    if ((obj->nodesetval == NULL)  || (obj->nodesetval->nodeNr < 0)) {
        xmlXPathFreeObject(obj);
        return 0;
    }

    ret = obj->nodesetval->nodeNr;
    if (list != NULL && ret) {
        *list = g_new0(xmlNodePtr, ret);
        memcpy(*list, obj->nodesetval->nodeTab, ret * sizeof(xmlNodePtr));
    }
    xmlXPathFreeObject(obj);
    return ret;
}


/**
 * catchXMLError:
 *
 * Called from SAX on parsing errors in the XML.
 *
 * This version is heavily based on xmlParserPrintFileContextInternal from libxml2.
 */
static void
catchXMLError(void *ctx, const char *msg G_GNUC_UNUSED, ...)
{
    xmlParserCtxtPtr ctxt = (xmlParserCtxtPtr) ctx;

    const xmlChar *cur, *base;
    unsigned int n, col;        /* GCC warns if signed, because compared with sizeof() */
    int domcode = VIR_FROM_XML;
    g_auto(virBuffer) buf = VIR_BUFFER_INITIALIZER;
    g_autofree char *contextstr = NULL;
    g_autofree char *pointerstr = NULL;


    /* conditions for error printing */
    if (!ctxt ||
        (virGetLastErrorCode()) ||
        ctxt->input == NULL ||
        ctxt->lastError.level != XML_ERR_FATAL ||
        ctxt->lastError.message == NULL)
        return;

    if (ctxt->_private)
        domcode = ((struct virParserData *) ctxt->_private)->domcode;


    cur = ctxt->input->cur;
    base = ctxt->input->base;

    /* skip backwards over any end-of-lines */
    while ((cur > base) && ((*(cur) == '\n') || (*(cur) == '\r')))
        cur--;

    /* search backwards for beginning-of-line (to max buff size) */
    while ((cur > base) && (*(cur) != '\n') && (*(cur) != '\r'))
        cur--;
    if ((*(cur) == '\n') || (*(cur) == '\r')) cur++;

    /* calculate the error position in terms of the current position */
    col = ctxt->input->cur - cur;

    /* search forward for end-of-line (to max buff size) */
    /* copy selected text to our buffer */
    while ((*cur != 0) && (*(cur) != '\n') && (*(cur) != '\r'))
        virBufferAddChar(&buf, *cur++);

    /* create blank line with problem pointer */
    contextstr = virBufferContentAndReset(&buf);

    /* (leave buffer space for pointer + line terminator) */
    for  (n = 0; (n<col) && (contextstr[n] != 0); n++) {
        if (contextstr[n] == '\t')
            virBufferAddChar(&buf, '\t');
        else
            virBufferAddChar(&buf, '-');
    }

    virBufferAddChar(&buf, '^');

    pointerstr = virBufferContentAndReset(&buf);

    if (ctxt->lastError.file) {
        virGenericReportError(domcode, VIR_ERR_XML_DETAIL,
                              _("%s:%d: %s%s\n%s"),
                              ctxt->lastError.file,
                              ctxt->lastError.line,
                              ctxt->lastError.message,
                              contextstr,
                              pointerstr);
    } else {
        virGenericReportError(domcode, VIR_ERR_XML_DETAIL,
                              _("at line %d: %s%s\n%s"),
                              ctxt->lastError.line,
                              ctxt->lastError.message,
                              contextstr,
                              pointerstr);
    }
}

/**
 * virXMLParseHelper:
 * @domcode: error domain of the caller, usually VIR_FROM_THIS
 * @filename: file to be parsed or NULL if string parsing is requested
 * @xmlStr: XML string to be parsed in case filename is NULL
 * @url: URL of XML document for string parser
 * @ctxt: optional pointer to populate with new context pointer
 *
 * Parse XML document provided either as a file or a string. The function
 * guarantees that the XML document contains a root element.
 *
 * Returns parsed XML document.
 */
xmlDocPtr
virXMLParseHelper(int domcode,
                  const char *filename,
                  const char *xmlStr,
                  const char *url,
                  xmlXPathContextPtr *ctxt)
{
    struct virParserData private;
    xmlParserCtxtPtr pctxt;
    xmlDocPtr xml = NULL;

    /* Set up a parser context so we can catch the details of XML errors. */
    pctxt = xmlNewParserCtxt();
    if (!pctxt || !pctxt->sax)
        abort();

    private.domcode = domcode;
    pctxt->_private = &private;
    pctxt->sax->error = catchXMLError;

    if (filename) {
        xml = xmlCtxtReadFile(pctxt, filename, NULL,
                              XML_PARSE_NONET |
                              XML_PARSE_NOWARNING);
    } else {
        xml = xmlCtxtReadDoc(pctxt, BAD_CAST xmlStr, url, NULL,
                             XML_PARSE_NONET |
                             XML_PARSE_NOWARNING);
    }
    if (!xml)
        goto error;

    if (xmlDocGetRootElement(xml) == NULL) {
        virGenericReportError(domcode, VIR_ERR_INTERNAL_ERROR,
                              "%s", _("missing root element"));
        goto error;
    }

    if (ctxt) {
        if (!(*ctxt = virXMLXPathContextNew(xml)))
            goto error;

        (*ctxt)->node = xmlDocGetRootElement(xml);
    }

 cleanup:
    xmlFreeParserCtxt(pctxt);

    return xml;

 error:
    xmlFreeDoc(xml);
    xml = NULL;

    if (virGetLastErrorCode() == VIR_ERR_OK) {
        virGenericReportError(domcode, VIR_ERR_XML_ERROR,
                              _("failed to parse xml document '%s'"),
                              filename ? filename : "[inline data]");
    }
    goto cleanup;
}

const char *virXMLPickShellSafeComment(const char *str1, const char *str2)
{
    if (str1 && !strpbrk(str1, "\r\t\n !\"#$&'()*;<>?[\\]^`{|}~") &&
        !strstr(str1, "--"))
        return str1;
    if (str2 && !strpbrk(str2, "\r\t\n !\"#$&'()*;<>?[\\]^`{|}~") &&
        !strstr(str2, "--"))
        return str2;
    return NULL;
}

static int virXMLEmitWarning(int fd,
                             const char *name,
                             const char *cmd)
{
    size_t len;
    const char *prologue =
        "<!--\n"
        "WARNING: THIS IS AN AUTO-GENERATED FILE. CHANGES TO IT ARE LIKELY TO BE\n"
        "OVERWRITTEN AND LOST. Changes to this xml configuration should be made using:\n"
        "  virsh ";
    const char *epilogue =
        "\n"
        "or other application using the libvirt API.\n"
        "-->\n\n";

    if (fd < 0 || !cmd) {
        errno = EINVAL;
        return -1;
    }

    len = strlen(prologue);
    if (safewrite(fd, prologue, len) != len)
        return -1;

    len = strlen(cmd);
    if (safewrite(fd, cmd, len) != len)
        return -1;

    if (name) {
        if (safewrite(fd, " ", 1) != 1)
            return -1;

        len = strlen(name);
        if (safewrite(fd, name, len) != len)
            return -1;
    }

    len = strlen(epilogue);
    if (safewrite(fd, epilogue, len) != len)
        return -1;

    return 0;
}


struct virXMLRewriteFileData {
    const char *warnName;
    const char *warnCommand;
    const char *xml;
};

static int
virXMLRewriteFile(int fd, const void *opaque)
{
    const struct virXMLRewriteFileData *data = opaque;

    if (data->warnCommand) {
        if (virXMLEmitWarning(fd, data->warnName, data->warnCommand) < 0)
            return -1;
    }

    if (safewrite(fd, data->xml, strlen(data->xml)) < 0)
        return -1;

    return 0;
}

int
virXMLSaveFile(const char *path,
               const char *warnName,
               const char *warnCommand,
               const char *xml)
{
    struct virXMLRewriteFileData data = { warnName, warnCommand, xml };

    return virFileRewrite(path, S_IRUSR | S_IWUSR, virXMLRewriteFile, &data);
}

/**
 * virXMLNodeToString: convert an XML node ptr to an XML string
 *
 * Returns the XML string of the document or NULL on error.
 * The caller has to free the string.
 */
char *
virXMLNodeToString(xmlDocPtr doc,
                   xmlNodePtr node)
{
    g_autoptr(xmlBuffer) xmlbuf = virXMLBufferCreate();

    if (xmlNodeDump(xmlbuf, doc, node, 0, 1) == 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("failed to convert the XML node tree"));
        return NULL;
    }

    return g_strdup((const char *)xmlBufferContent(xmlbuf));
}


/**
 * virXMLNodeNameEqual:
 * @node: xml Node pointer to check
 * @name: name of the @node
 *
 * Compares the @node name with @name.
 */
bool
virXMLNodeNameEqual(xmlNodePtr node,
                    const char *name)
{
    return xmlStrEqual(node->name, BAD_CAST name);
}


typedef int (*virXMLForeachCallback)(xmlNodePtr node,
                                     void *opaque);

static int
virXMLForeachNode(xmlNodePtr root,
                  virXMLForeachCallback cb,
                  void *opaque)
{
    xmlNodePtr next;
    int ret;

    for (next = root; next; next = next->next) {
        if ((ret = cb(next, opaque)) != 0)
            return ret;

        /* recurse into children */
        if (next->children) {
            if ((ret = virXMLForeachNode(next->children, cb, opaque)) != 0)
                return ret;
        }
    }

    return 0;
}


static int
virXMLRemoveElementNamespace(xmlNodePtr node,
                             void *opaque)
{
    const char *uri = opaque;

    if (node->ns &&
        STREQ_NULLABLE((const char *)node->ns->href, uri))
        xmlSetNs(node, NULL);
    return 0;
}


xmlNodePtr
virXMLFindChildNodeByNs(xmlNodePtr root,
                        const char *uri)
{
    xmlNodePtr next;

    if (!root)
        return NULL;

    for (next = root->children; next; next = next->next) {
        if (next->ns &&
            STREQ_NULLABLE((const char *) next->ns->href, uri))
            return next;
    }

    return NULL;
}


/**
 * virXMLExtractNamespaceXML: extract a sub-namespace of XML as string
 */
int
virXMLExtractNamespaceXML(xmlNodePtr root,
                          const char *uri,
                          char **doc)
{
    xmlNodePtr node;
    xmlNodePtr nodeCopy = NULL;
    xmlNsPtr actualNs;
    xmlNsPtr prevNs = NULL;
    char *xmlstr = NULL;
    int ret = -1;

    if (!(node = virXMLFindChildNodeByNs(root, uri))) {
        /* node not found */
        ret = 1;
        goto cleanup;
    }

    /* copy the node so that we can modify the namespace */
    if (!(nodeCopy = xmlCopyNode(node, 1))) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Failed to copy XML node"));
        goto cleanup;
    }

    virXMLForeachNode(nodeCopy, virXMLRemoveElementNamespace,
                      (void *)uri);

    /* remove the namespace declaration
     *  - it's only a single linked list ... doh */
    for (actualNs = nodeCopy->nsDef; actualNs; actualNs = actualNs->next) {
        if (STREQ_NULLABLE((const char *)actualNs->href, uri)) {

            /* unlink */
            if (prevNs)
                prevNs->next = actualNs->next;
            else
                nodeCopy->nsDef = actualNs->next;

            /* discard */
            xmlFreeNs(actualNs);
            break;
        }

        prevNs = actualNs;
    }

    if (!(xmlstr = virXMLNodeToString(nodeCopy->doc, nodeCopy)))
        goto cleanup;

    ret = 0;

 cleanup:
    if (doc)
        *doc = xmlstr;
    else
        VIR_FREE(xmlstr);
    xmlFreeNode(nodeCopy);
    return ret;
}


static int
virXMLAddElementNamespace(xmlNodePtr node,
                          void *opaque)
{
    xmlNsPtr ns = opaque;

    if (!node->ns)
        xmlSetNs(node, ns);

    return 0;
}


int
virXMLInjectNamespace(xmlNodePtr node,
                      const char *uri,
                      const char *key)
{
    xmlNsPtr ns;

    if (xmlValidateNCName((const unsigned char *)key, 1) != 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("failed to validate prefix for a new XML namespace"));
        return -1;
    }

    if (!(ns = xmlNewNs(node, (const unsigned char *)uri, (const unsigned char *)key))) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("failed to create a new XML namespace"));
        return -1;
    }

    virXMLForeachNode(node, virXMLAddElementNamespace, ns);

    return 0;
}

/**
 * virXMLNodeSanitizeNamespaces()
 * @node: Sanitize the namespaces for this node
 *
 * This function removes subnodes in node that share the namespace.
 * The first instance of every duplicate namespace is kept.
 * Additionally nodes with no namespace are deleted.
 */
void
virXMLNodeSanitizeNamespaces(xmlNodePtr node)
{
    xmlNodePtr child;
    xmlNodePtr next;
    xmlNodePtr dupl;

    if (!node)
        return;

    child = node->children;
    while (child) {
        /* remove subelements that don't have any namespace at all */
        if (!child->ns || !child->ns->href) {
            dupl = child;
            child = child->next;

            xmlUnlinkNode(dupl);
            xmlFreeNode(dupl);
            continue;
        }

        /* check that every other child of @root doesn't share the namespace of
         * the current one and delete them possibly */
        next = child->next;
        while (next) {
            dupl = NULL;

            if (child->ns && next->ns &&
                STREQ_NULLABLE((const char *) child->ns->href,
                               (const char *) next->ns->href))
                dupl = next;

            next = next->next;
            if (dupl) {
                xmlUnlinkNode(dupl);
                xmlFreeNode(dupl);
            }
        }
        child = child->next;
    }
}


static void catchRNGError(void *ctx,
                          const char *msg,
                          ...)
{
    virBuffer *buf = ctx;
    va_list args;

    va_start(args, msg);
    VIR_WARNINGS_NO_PRINTF;
    virBufferVasprintf(buf, msg, args);
    VIR_WARNINGS_RESET;
    va_end(args);
}


static void ignoreRNGError(void *ctx G_GNUC_UNUSED,
                           const char *msg G_GNUC_UNUSED,
                           ...)
{}


virXMLValidator *
virXMLValidatorInit(const char *schemafile)
{
    virXMLValidator *validator = NULL;

    validator = g_new0(virXMLValidator, 1);

    validator->schemafile = g_strdup(schemafile);

    if (!(validator->rngParser =
          xmlRelaxNGNewParserCtxt(validator->schemafile))) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Unable to create RNG parser for %s"),
                       validator->schemafile);
        goto error;
    }

    xmlRelaxNGSetParserErrors(validator->rngParser,
                              catchRNGError,
                              ignoreRNGError,
                              &validator->buf);

    if (!(validator->rng = xmlRelaxNGParse(validator->rngParser))) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Unable to parse RNG %s: %s"),
                       validator->schemafile,
                       virBufferCurrentContent(&validator->buf));
        goto error;
    }

    if (!(validator->rngValid = xmlRelaxNGNewValidCtxt(validator->rng))) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Unable to create RNG validation context %s"),
                       validator->schemafile);
        goto error;
    }

    xmlRelaxNGSetValidErrors(validator->rngValid,
                             catchRNGError,
                             ignoreRNGError,
                             &validator->buf);
    return validator;

 error:
    virXMLValidatorFree(validator);
    return NULL;
}


int
virXMLValidatorValidate(virXMLValidator *validator,
                        xmlDocPtr doc)
{
    if (xmlRelaxNGValidateDoc(validator->rngValid, doc) != 0) {
        virReportError(VIR_ERR_XML_INVALID_SCHEMA,
                       _("Unable to validate doc against %s\n%s"),
                       validator->schemafile,
                       virBufferCurrentContent(&validator->buf));
        return -1;
    }

    return 0;
}


int
virXMLValidateAgainstSchema(const char *schemafile,
                            xmlDocPtr doc)
{
    virXMLValidator *validator = NULL;
    int ret = -1;

    if (!(validator = virXMLValidatorInit(schemafile)))
        return -1;

    if (virXMLValidatorValidate(validator, doc) < 0)
        goto cleanup;

    ret = 0;
 cleanup:
    virXMLValidatorFree(validator);
    return ret;
}


int
virXMLValidateNodeAgainstSchema(const char *schemafile,
                                xmlDocPtr doc,
                                xmlNodePtr node)
{
    xmlNodePtr root;
    int ret;

    root = xmlDocSetRootElement(doc, node);
    ret = virXMLValidateAgainstSchema(schemafile, doc);
    xmlDocSetRootElement(doc, root);
    return ret;
}


void
virXMLValidatorFree(virXMLValidator *validator)
{
    if (!validator)
        return;

    g_free(validator->schemafile);
    virBufferFreeAndReset(&validator->buf);
    xmlRelaxNGFreeParserCtxt(validator->rngParser);
    xmlRelaxNGFreeValidCtxt(validator->rngValid);
    xmlRelaxNGFree(validator->rng);
    g_free(validator);
}


/**
 * virXMLFormatElement
 * @buf: the parent buffer where the element will be placed
 * @name: the name of the element
 * @attrBuf: buffer with attributes for element, may be NULL
 * @childBuf: buffer with child elements, may be NULL
 *
 * Helper to format element where attributes or child elements
 * are optional and may not be formatted.  If both @attrBuf and
 * @childBuf are NULL or are empty buffers the element is not
 * formatted.
 *
 * Both passed buffers are always consumed and freed.
 */
void
virXMLFormatElement(virBuffer *buf,
                    const char *name,
                    virBuffer *attrBuf,
                    virBuffer *childBuf)
{
    if ((!attrBuf || virBufferUse(attrBuf) == 0) &&
        (!childBuf || virBufferUse(childBuf) == 0))
        return;

    virBufferAsprintf(buf, "<%s", name);

    if (attrBuf && virBufferUse(attrBuf) > 0)
        virBufferAddBuffer(buf, attrBuf);

    if (childBuf && virBufferUse(childBuf) > 0) {
        virBufferAddLit(buf, ">\n");
        virBufferAddBuffer(buf, childBuf);
        virBufferAsprintf(buf, "</%s>\n", name);
    } else {
        virBufferAddLit(buf, "/>\n");
    }

    virBufferFreeAndReset(attrBuf);
    virBufferFreeAndReset(childBuf);
}


void
virXPathContextNodeRestore(virXPathContextNodeSave *save)
{
    if (!save->ctxt)
        return;

    save->ctxt->node = save->node;
}


void
virXMLNamespaceFormatNS(virBuffer *buf,
                        virXMLNamespace const *ns)
{
    virBufferAsprintf(buf, " xmlns:%s='%s'", ns->prefix, ns->uri);
}


int
virXMLNamespaceRegister(xmlXPathContextPtr ctxt,
                        virXMLNamespace const *ns)
{
    if (xmlXPathRegisterNs(ctxt,
                           BAD_CAST ns->prefix,
                           BAD_CAST ns->uri) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Failed to register xml namespace '%s'"),
                       ns->uri);
        return -1;
    }

    return 0;
}


/**
 * virParseScaledValue:
 * @xpath: XPath to memory amount
 * @units_xpath: XPath to units attribute
 * @ctxt: XPath context
 * @val: scaled value is stored here
 * @scale: default scale for @val
 * @max: maximal @val allowed
 * @required: is the value required?
 *
 * Parse a value located at @xpath within @ctxt, and store the
 * result into @val. The value is scaled by units located at
 * @units_xpath (or the 'unit' attribute under @xpath if
 * @units_xpath is NULL). If units are not present, the default
 * @scale is used. If @required is set, then the value must
 * exist; otherwise, the value is optional. The resulting value
 * is in bytes.
 *
 * Returns 1 on success,
 *         0 if the value was not present and !@required,
 *         -1 on failure after issuing error.
 */
int
virParseScaledValue(const char *xpath,
                    const char *units_xpath,
                    xmlXPathContextPtr ctxt,
                    unsigned long long *val,
                    unsigned long long scale,
                    unsigned long long max,
                    bool required)
{
    unsigned long long bytes;
    g_autofree char *xpath_full = NULL;
    g_autofree char *unit = NULL;
    g_autofree char *bytes_str = NULL;

    *val = 0;
    xpath_full = g_strdup_printf("string(%s)", xpath);

    bytes_str = virXPathString(xpath_full, ctxt);
    if (!bytes_str) {
        if (!required)
            return 0;
        virReportError(VIR_ERR_XML_ERROR,
                       _("missing element or attribute '%s'"),
                       xpath);
        return -1;
    }
    VIR_FREE(xpath_full);

    if (virStrToLong_ullp(bytes_str, NULL, 10, &bytes) < 0) {
        virReportError(VIR_ERR_XML_ERROR,
                       _("Invalid value '%s' for element or attribute '%s'"),
                       bytes_str, xpath);
        return -1;
    }

    if (units_xpath)
         xpath_full = g_strdup_printf("string(%s)", units_xpath);
    else
         xpath_full = g_strdup_printf("string(%s/@unit)", xpath);
    unit = virXPathString(xpath_full, ctxt);

    if (virScaleInteger(&bytes, unit, scale, max) < 0)
        return -1;

    *val = bytes;
    return 1;
}


xmlBufferPtr
virXMLBufferCreate(void)
{
    xmlBufferPtr ret;

    if (!(ret = xmlBufferCreate()))
        abort();

    return ret;
}


xmlNodePtr
virXMLNewNode(xmlNsPtr ns,
              const char *name)
{
    xmlNodePtr ret;

    if (!(ret = xmlNewNode(ns, BAD_CAST name)))
        abort();

    return ret;
}
