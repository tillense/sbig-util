/*****************************************************************************\
 *  Copyright (c) 2014 Jim Garlick All rights reserved.
 *
 *  This file is part of the sbig-util.
 *  For details, see https://github.com/garlick/sbig-util.
 *
 *  This program is free software; you can redistribute it and/or modify it
 *  under the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 3 of the license, or (at your option)
 *  any later version.
 *
 *  sbig-util is distributed in the hope that it will be useful, but WITHOUT
 *  ANY WARRANTY; without even the IMPLIED WARRANTY OF MERCHANTABILITY or
 *  FITNESS FOR A PARTICULAR PURPOSE.  See the terms and conditions of the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  59 Temple Place, Suite 330, Boston, MA 02111-1307 USA.
 *  See also:  http://www.gnu.org/licenses/
\*****************************************************************************/

#if HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <errno.h>
#include <string.h>
#include <stdbool.h>
#include <arpa/inet.h> /* htons */
#include <time.h>
#include <math.h>

#include "handle.h"
#include "handle_impl.h"
#include "sbigudrv.h"
#include "sbig.h"

#include "src/common/libutil/bcd.h"
#include "src/common/libutil/color.h"
#include "src/common/libutil/xzmalloc.h"

struct sbig_ccd {
    sbig_t *sb;
    CCD_REQUEST ccd;
    ABG_STATE7 abg_mode;
    READOUT_BINNING_MODE readout_mode;
    SHUTTER_COMMAND shutter_mode;
    GetCCDInfoResults0 info0;
    ushort top, left, height, width;
    ushort *frame;
    ulong exp_flags;
    double exposureTime;
    time_t exposureStart;
    CFW_POSITION last_cfw_position;
    int restore_cfw_position:1;
    int has_eshutter:1;
    int color_bayer:1;
    int color_truesense:1;
};

static int lookup_roinfo (sbig_ccd_t *ccd, READOUT_BINNING_MODE mode)
{
    int i;
    for (i = 0; i < ccd->info0.readoutModes; i++)
        if (ccd->info0.readoutInfo[i].mode == (mode & 0x00FF)) // check only low byte because of vertical binning
            return i;
    return -1;
}

static int lookup_roheight (sbig_ccd_t *ccd, READOUT_BINNING_MODE mode)
{
    ushort height;
    int ro_index = lookup_roinfo (ccd, mode);

    if (ro_index >= 0) {
        // check for ro modes with vertical binning
        if ( ( (mode & 0x00FF ) == RM_NX1 ) ||
            ( (mode & 0x00FF ) == RM_NX2 ) ||
            ( (mode & 0x00FF ) == RM_NX3 ) ) {
            // get vertical height = max height / vertical binning (taken from high byte of ro mode)
            height = ccd->info0.readoutInfo[0].height / ( (mode & 0xFF00) >> 8);
            if ( height > 0 )
                return height;
        } else {
            return ccd->height = ccd->info0.readoutInfo[ro_index].height;
        }
    }

    return -1;
}

static void realloc_frame (sbig_ccd_t *ccd)
{
    if (ccd->frame)
        free (ccd->frame);
    ccd->frame = xzmalloc (sizeof (*ccd->frame) * ccd->height * ccd->width);
}

/* FIXME: PixCel255/237 doesn't support info0 on tracking ccd
 */
int sbig_ccd_create (sbig_t *sb, CCD_REQUEST chip, sbig_ccd_t **ccdp)
{
    sbig_ccd_t *ccd = xzmalloc (sizeof (*ccd));
    GetCCDInfoResults4 info4;
    GetCCDInfoResults6 info6;
    int e;

    ccd->ccd = chip;
    ccd->sb = sb;

    e = sbig_ccd_get_info0 (ccd, &ccd->info0);
    if (e != CE_NO_ERROR) {
        free (ccd);
        return e;
    }
    e = sbig_ccd_get_info4 (ccd, &info4);
    if (e != CE_NO_ERROR) {
        free (ccd);
        return e;
    }
    e = sbig_ccd_get_info6 (ccd, &info6);
    if (e != CE_NO_ERROR) {
        free (ccd);
        return e;
    }

    if ((info4.capabilitiesBits & CB_CCD_ESHUTTER_MASK) == CB_CCD_ESHUTTER_YES)
        ccd->has_eshutter = 1;

    ccd->abg_mode = ABG_LOW7;            /* ABG shut off during exposure */
    ccd->shutter_mode = SC_OPEN_SHUTTER; /* open during exp, close during r/o */

    /* Default to the first readout mode camera selects.
     * On the ST-8 at least this is RM_1X1.
     * N.B. readout_mode == readoutInfo->mode not readoutInfo index.
     * Use lookup_roinfo() to find index of a particular RM_ mode.
     */
    assert (ccd->info0.readoutModes > 0);
    ccd->readout_mode = ccd->info0.readoutInfo[0].mode;

    /* Default to full frame for this readout mode.
     * top, left, height, width are in binned pixels
     */
    ccd->top = 0;
    ccd->left = 0;
    ccd->height = ccd->info0.readoutInfo[0].height;
    ccd->width = ccd->info0.readoutInfo[0].width;
    realloc_frame (ccd);

    /* Note that this is a one-shot color camera with a Bayer matrix.
     */
    if ((info6.ccdBits & 0x3) == 1)
        ccd->color_bayer = 1;
    else if ((info6.ccdBits & 0x3) == 3)
        ccd->color_truesense = 1;

    *ccdp = ccd;
    return CE_NO_ERROR;
}

void sbig_ccd_destroy (sbig_ccd_t *ccd)
{
    if (ccd->frame)
        free (ccd->frame);
    free (ccd);
}

int sbig_ccd_get_info0 (sbig_ccd_t *ccd, GetCCDInfoResults0 *info)
{
    GetCCDInfoParams in;

    if (ccd->ccd == CCD_IMAGING)
        in.request = CCD_INFO_IMAGING;
    else if (ccd->ccd == CCD_TRACKING)
        in.request = CCD_INFO_TRACKING;
    else
        return CE_BAD_PARAMETER;
    return ccd->sb->fun (CC_GET_CCD_INFO, &in, info);
}

int sbig_ccd_get_info2 (sbig_ccd_t *ccd, GetCCDInfoResults2 *info)
{
    GetCCDInfoParams in;

    if (ccd->ccd == CCD_IMAGING)
        in.request = CCD_INFO_EXTENDED;
    else
        return CE_BAD_PARAMETER;
    return ccd->sb->fun (CC_GET_CCD_INFO, &in, info);
}

int sbig_ccd_get_info3 (sbig_ccd_t *ccd, GetCCDInfoResults3 *info)
{
    GetCCDInfoParams in;

    if (ccd->ccd == CCD_IMAGING)
        in.request = CCD_INFO_EXTENDED_5C;
    else
        return CE_BAD_PARAMETER;
    return ccd->sb->fun (CC_GET_CCD_INFO, &in, info);
}

int sbig_ccd_get_info4 (sbig_ccd_t *ccd, GetCCDInfoResults4 *info)
{
    GetCCDInfoParams in;

    if (ccd->ccd == CCD_IMAGING)
        in.request = CCD_INFO_EXTENDED2_IMAGING;
    else if (ccd->ccd == CCD_TRACKING)
        in.request = CCD_INFO_EXTENDED2_TRACKING;
    else
        return CE_BAD_PARAMETER;
    return ccd->sb->fun (CC_GET_CCD_INFO, &in, info);
}

int sbig_ccd_get_info6 (sbig_ccd_t *ccd, GetCCDInfoResults6 *info)
{
    GetCCDInfoParams in;

    if (ccd->ccd == CCD_IMAGING)
        in.request = CCD_INFO_EXTENDED3;
    else
        return CE_BAD_PARAMETER;
    return ccd->sb->fun (CC_GET_CCD_INFO, &in, info);
}

int sbig_ccd_set_abg_mode (sbig_ccd_t *ccd, ABG_STATE7 mode)
{
    ccd->abg_mode = mode;
    return CE_NO_ERROR;
}

int sbig_ccd_get_abg_mode (sbig_ccd_t *ccd, ABG_STATE7 *modep)
{
    *modep = ccd->abg_mode;
    return CE_NO_ERROR;
}

int sbig_ccd_set_readout_mode (sbig_ccd_t *ccd, READOUT_BINNING_MODE mode)
{
    int ro_index = lookup_roinfo (ccd, mode);
    ushort ro_height = lookup_roheight (ccd, mode);

    if (ro_index == -1 || ro_height == -1)
        return CE_BAD_PARAMETER;

    ccd->readout_mode = mode;
    ccd->top = 0;
    ccd->left = 0;
    ccd->height = ro_height;
    ccd->width = ccd->info0.readoutInfo[ro_index].width;
    realloc_frame (ccd);
    return CE_NO_ERROR;
}

int sbig_ccd_get_readout_mode (sbig_ccd_t *ccd, READOUT_BINNING_MODE *modep)
{
    *modep = ccd->readout_mode;
    return CE_NO_ERROR;
}

int sbig_ccd_set_shutter_mode (sbig_ccd_t *ccd, SHUTTER_COMMAND mode)
{
    ccd->shutter_mode = mode;
    return CE_NO_ERROR;
}

int sbig_ccd_get_shutter_mode (sbig_ccd_t *ccd, SHUTTER_COMMAND *modep)
{
    *modep = ccd->shutter_mode;
    return CE_NO_ERROR;
}

/* Calculate centered fractional edge for one dimension of subframe.
 * F = fraction of the area of the rectangle to use in the subframe.
 * m = full frame maximum.
 * a = origin of the subframe
 * Return value = length of the subframe.
 * +------------+----------+-------------+
 * 0            a          b             m
 */
static double cfe (double m, double F, double *a)
{
    double b = m * (sqrt(F) + 1) / 2;
    *a = m - b;
    return (b - *a);
}

/* Adjust subframe if it lands on an odd offset so that 2x2 bayer matrix
 * always starts on the same color
 */
static void align_bayer_matrix (sbig_ccd_t *ccd)
{
    if (ccd->top % 2 != 0)
        ccd->top++;
    if (ccd->left % 2 != 0)
        ccd->left++;
}

int sbig_ccd_set_partial_frame (sbig_ccd_t *ccd, double part)
{
    int ro_index = lookup_roinfo (ccd, ccd->readout_mode);
    double top, left;

    ccd->height = cfe (lookup_roheight(ccd, ccd->readout_mode), part, &top);
    ccd->top = top;
    ccd->width = cfe (ccd->info0.readoutInfo[ro_index].width, part, &left);
    ccd->left = left;
    if (ccd->color_bayer)
        align_bayer_matrix (ccd);
    realloc_frame (ccd);
    return CE_NO_ERROR;
}

int sbig_ccd_set_window (sbig_ccd_t *ccd, ushort top, ushort left,
                         ushort height, ushort width)
{
    if (top > ccd->height || left > ccd->width || height > ccd->height
                                               || width > ccd->width)
        return CE_BAD_PARAMETER;
    ccd->top = top;
    ccd->left = left;
    ccd->height = height;
    ccd->width = width;
    if (ccd->color_bayer)
        align_bayer_matrix (ccd);
    realloc_frame (ccd);
    return CE_NO_ERROR;
}

int sbig_ccd_get_window (sbig_ccd_t *ccd, ushort *topp, ushort *leftp,
                         ushort *heightp, ushort *widthp)
{
    *topp = ccd->top;
    *leftp = ccd->left;
    *heightp = ccd->height;
    *widthp = ccd->width;
    return CE_NO_ERROR;
}

int sbig_ccd_set_exposure_flags (sbig_ccd_t *ccd, ulong flags)
{
    if ((flags & EXP_TIME_MASK) || (flags & EXP_MS_EXPOSURE))
        return CE_BAD_PARAMETER;
    ccd->exp_flags |= flags;
    return CE_NO_ERROR;
}

int sbig_ccd_clr_exposure_flags (sbig_ccd_t *ccd, ulong flags)
{
    if ((flags & EXP_TIME_MASK) || (flags & EXP_MS_EXPOSURE))
        return CE_BAD_PARAMETER;
    ccd->exp_flags &= ~flags;
    return CE_NO_ERROR;
}

/* Min exposure in seconds
 * FIXME: I've been conservative in grouping the ? cameras with ST7.
 */
static double min_exposure (sbig_ccd_t *ccd)
{
    double m;

    switch (ccd->info0.cameraType) {
        case ST402_CAMERA:
            m = 1E-2*MIN_ST402_EXPOSURE;
            break;
        case STX_CAMERA:
            m = 1E-2*MIN_STX_EXPOSURE;
            break;
        case STT_CAMERA:
            m = 1E-2*MIN_STT_EXPOSURE;
            break;
        case STI_CAMERA:
            m = 1E-3*MIN_STU_EXPOSURE;
            break;
        case STF_CAMERA:
            //m = 1E-2*MIN_STF3200_EXPOSURE; // ?
            //m = 1E-3*MIN_STF8050_EXPOSURE; // ?
            //m = 1E-3*MIN_STF4070_EXPOSURE; // ?
            m = 1E-2*MIN_STF8300_EXPOSURE; // largest of the STL's
            break;
        case ST7_CAMERA:
        case ST8_CAMERA:
        case ST9_CAMERA:
        case ST10_CAMERA:
        case ST1K_CAMERA:
        case ST4K_CAMERA: // ?
        case ST5C_CAMERA: // ? (PixCel)
        case ST237_CAMERA: // ? (PixCel)
        case TCE_CONTROLLER: // ?
        case STV_CAMERA: // ?
        case ST2K_CAMERA: // ?
        case STL_CAMERA: // ?
        default:
            m = 1E-2*MIN_ST7_EXPOSURE; // 0.12 == 120 msec
            break;
    }
    /* Fudge possibly conservative numbers above if we have an e-shutter.
     */
    if (ccd->has_eshutter)
        m = 1E-3;

    return m;
}

int sbig_ccd_start_exposure (sbig_ccd_t *ccd, unsigned short flags,
                             double exposureTime)
{
    StartExposureParams2 in = { .ccd = ccd->ccd | flags,
                                .abgState = ccd->abg_mode,
                                .openShutter = ccd->shutter_mode,
                                .readoutMode = ccd->readout_mode,
                                .top = ccd->top, .left = ccd->left,
                                .height = ccd->height, .width = ccd->width };

    if (exposureTime < min_exposure (ccd) || exposureTime*100 > 0x00ffffff)
        return CE_BAD_PARAMETER;
    if (exposureTime < 0.01 && ccd->has_eshutter) {
        in.exposureTime = exposureTime * 1000.0;
        in.exposureTime |= EXP_MS_EXPOSURE;
    } else
        in.exposureTime = exposureTime * 100.0;
    in.exposureTime |= ccd->exp_flags;
    ccd->exposureTime = exposureTime; /* leave it here for stats later */
    ccd->exposureStart = time (NULL);

    /* ST-5C and ST-237 have internal filter wheel instead of shutter.
     * (shutter_mode parameter is ignored).  for CFW5: 1=open, 2=closed.
     * If filter is repositioned in start_exposure, restore it in end_exposure.
     */
    ccd->restore_cfw_position = 0;
    if (ccd->info0.cameraType == ST5C_CAMERA
                                || ccd->info0.cameraType == ST237_CAMERA) {
        CFW_STATUS status;
        int e;
	CFW_ERROR cfwerr;

        e = sbig_cfw_query (ccd->sb, &status, &ccd->last_cfw_position, &cfwerr);
        if (e != CE_NO_ERROR)
            return e;
        if (ccd->shutter_mode == SC_CLOSE_SHUTTER
                                            && ccd->last_cfw_position != 2) {
            if ((e = sbig_cfw_goto (ccd->sb, 2, &cfwerr)) != CE_NO_ERROR)
                return e;
            ccd->restore_cfw_position = 1;
        }
        else if (ccd->shutter_mode == SC_OPEN_SHUTTER
                                            && ccd->last_cfw_position == 2) {
            if ((e = sbig_cfw_goto (ccd->sb, 1, &cfwerr)) != CE_NO_ERROR)
                return e;
            ccd->restore_cfw_position = 1;
        }
    }
    return ccd->sb->fun (CC_START_EXPOSURE2, &in, NULL);
}

int sbig_ccd_get_exposure_status (sbig_ccd_t *ccd, PAR_COMMAND_STATUS *sp)
{
    ushort status;
    int e = sbig_query_cmd_status (ccd->sb, CC_START_EXPOSURE2, &status);

    if (e == CE_NO_ERROR) {
        if (ccd->ccd == CCD_IMAGING)
            *sp = status & 3;
        else
            *sp = (status >> 2) & 3;
    }
    return e;
}

int sbig_ccd_end_exposure (sbig_ccd_t *ccd, ushort flags)
{
    EndExposureParams in = { .ccd = ccd->ccd | flags };
    CFW_ERROR cfwerr;

    if (ccd->restore_cfw_position) {
        int e = sbig_cfw_goto (ccd->sb, ccd->last_cfw_position, &cfwerr);
        if (e != CE_NO_ERROR)
            return e;
        ccd->restore_cfw_position = 0;
    }

    return ccd->sb->fun (CC_END_EXPOSURE, &in, NULL);
}

static int start_readout (sbig_ccd_t *ccd)
{
    StartReadoutParams in = { .ccd = ccd->ccd, .readoutMode = ccd->readout_mode,
                              .top = ccd->top, .left = ccd->left,
                              .height = ccd->height, .width = ccd->width };

    return ccd->sb->fun (CC_START_READOUT, &in, NULL);
}

/* On ST-7/8/etc, end_readout turns off CCD preamp and unfreezes TE if
 * autofreeze enabled
 */
static int end_readout (sbig_ccd_t *ccd)
{
    EndReadoutParams in = { .ccd = ccd->ccd };

    return ccd->sb->fun (CC_END_READOUT, &in, NULL);
}

static int readout_line (sbig_ccd_t *ccd, ushort start, ushort len, ushort *buf)
{
    ReadoutLineParams in = { .ccd = ccd->ccd, .readoutMode = ccd->readout_mode,
                             .pixelStart = start, .pixelLength = len };

    return ccd->sb->fun (CC_READOUT_LINE, &in, buf);
}

static int read_subtract_line (sbig_ccd_t *ccd, ushort start, ushort len, ushort *buf)
{
    ReadoutLineParams in = { .ccd = ccd->ccd, .readoutMode = ccd->readout_mode,
                             .pixelStart = start, .pixelLength = len };

    return ccd->sb->fun (CC_READ_SUBTRACT_LINE, &in, buf);
}

int sbig_ccd_readout (sbig_ccd_t *ccd)
{
    ushort *pp = ccd->frame;
    int i, e;

    assert (pp != NULL);

    e = start_readout (ccd);
    for (i = 0; e == CE_NO_ERROR && i < ccd->height; i++) {
        e = readout_line (ccd, ccd->left, ccd->width, pp);
        pp += ccd->width;
    }
    if (e == CE_NO_ERROR)
        e = end_readout (ccd);

    return e;
}

int sbig_ccd_readout_subtract (sbig_ccd_t *ccd)
{
    ushort *pp = ccd->frame;
    int i, e;

    assert (pp != NULL);

    e = start_readout (ccd);
    for (i = 0; e == CE_NO_ERROR && i < ccd->height; i++) {
        e = read_subtract_line (ccd, ccd->left, ccd->width, pp);
        pp += ccd->width;
    }
    if (e == CE_NO_ERROR)
        e = end_readout (ccd);

    return e;
}

int sbig_ccd_color_convert (sbig_ccd_t *ccd, const char *method)
{
    if (!ccd->color_bayer) // FIXME: add support for Truesense (which cam?)
        return CE_BAD_PARAMETER;

    if (!strncasecmp (method, "monochrome", strlen (method))) {
        ushort *xframe;

        if (!(xframe = calloc (ccd->height*ccd->width, sizeof (ushort))))
            return CE_OS_ERROR;
        color_bayer_to_mono (ccd->frame, xframe, ccd->width, ccd->height);
        free (ccd->frame);
        ccd->frame = xframe;
        return CE_NO_ERROR;
    }
    return CE_BAD_PARAMETER;
}

/* These two functions presume that SBIGUdrv gave us unsigned shorts
 * in host byte order.
 */

int sbig_ccd_writemem (sbig_ccd_t *ccd, ushort *buf, int len)
{
    int i;

    if (len != ccd->height * ccd->width)
        return CE_BAD_PARAMETER;
    for (i = 0; i < len; i++)
        buf[i] = ccd->frame[i];
    return CE_NO_ERROR;
}

static int add_comments (sbig_ccd_t *ccd, FILE *f)
{
    int i = lookup_roinfo (ccd, ccd->readout_mode);

    if (fprintf (f, "P5 %d %d 65535\n", ccd->width, ccd->height) < 0)
        return -1;
    if (fprintf (f, "# SBIG %s\n", sbig_strcam (ccd->info0.cameraType)) < 0)
        return -1;
    if (fprintf (f, "# exposureTime %.3f seconds\n", ccd->exposureTime) < 0)
        return -1;
    if (fprintf (f, "# mode %s (%d x %d) %2.2f e-/ADU %3.2f x %-3.2f microns\n",
                 ccd->readout_mode == RM_1X1 ? "high" :
                 ccd->readout_mode == RM_2X2 ? "medium" :
                 ccd->readout_mode == RM_3X3 ? "low" : "other",
                 ccd->info0.readoutInfo[i].width,
                 ccd->info0.readoutInfo[i].height,
                 bcd2_2 (ccd->info0.readoutInfo[i].gain),
                 bcd6_2 (ccd->info0.readoutInfo[i].pixelWidth),
                 bcd6_2 (ccd->info0.readoutInfo[i].pixelHeight)) < 0)
        return -1;
    return 0;
}

int sbig_ccd_writepgm (sbig_ccd_t *ccd, const char *filename)
{
    ushort *pp = ccd->frame;
    ushort *nrow;
    FILE *f;
    int i, j;

    assert (pp != NULL);

    nrow = xzmalloc (sizeof (*nrow) * ccd->width);

    if (!(f = fopen (filename, "w+")))
        goto error;
    if (fprintf (f, "P5 %d %d 65535\n", ccd->width, ccd->height) < 0)
        goto error;
    if (add_comments (ccd, f) < 0)
        goto error;
    for (i = 0; i < ccd->height; i++) {
        for (j = 0; j < ccd->width; j++)
            nrow[j] = htons (*pp++);
        if (fwrite (nrow, sizeof (*nrow), ccd->width, f) < ccd->width)
            goto error;
    }
    if (fclose (f) != 0)
        goto error;
    return CE_NO_ERROR;
error:
    if (f)
        fclose (f);
    return CE_OS_ERROR;
}

ushort *sbig_ccd_get_data (sbig_ccd_t *ccd, ushort *height, ushort *width)
{
    *height = ccd->height;
    *width = ccd->width;
    return ccd->frame;
}

time_t sbig_ccd_get_start_time (sbig_ccd_t *ccd)
{
    return ccd->exposureStart;
}

double sbig_ccd_get_exposure_time (sbig_ccd_t *ccd)
{
    return ccd->exposureTime;
}

int sbig_ccd_get_max (sbig_ccd_t *ccd, ushort *maxp)
{
    int i, j;
    ushort max = 0;
    ushort *pp = ccd->frame;
    for (i = 0; i < ccd->height; i++) {
        for (j = 0; j < ccd->width; j++)
            if (*pp > max)
                max = *pp;
    }
    *maxp = max;
    return CE_NO_ERROR;
}

/* Borrowed from CSBIGImg::AutoBackgroundAndRange() (sdk/app).
 */
int sbig_ccd_auto_contrast (sbig_ccd_t *ccd, long *cblack, long *cwhite)
{
    ushort *pp = ccd->frame;
    ulong hist[4096];
    int i, j;
    ulong totalPixels, histSum;
    ulong s20, s99;
    ushort p20, p99;
    long back, range;

    // calculate the pixel histogram with 4096 bins
    memset(hist, 0, sizeof(hist));
    for (i = 0; i < ccd->height; i++)
            for (j = 0; j < ccd->width; j++)
                    hist[(*pp++) >> 4]++;

    // integrate the histogram and find the 20% and 99% points
    totalPixels = (unsigned long)ccd->width * ccd->height;
    s20 = (20 * totalPixels) / 100;
    s99 = (99 * totalPixels) / 100;
    histSum = 0;
    p20 = p99 = 65535;
    for (i = 0; i < 4096; i++) {
            histSum += hist[i];
            if (histSum >= s20 && p20 == 65535)
                    p20 = i;
            if (histSum >= s99 && p99 == 65535)
                    p99 = i;
    }

    // set the range to 110% of the difference between
    // the 99% and 20% histogram points, not letting
    // it be too low or overflow unsigned short
    range = (16L * (p99 - p20) * 11) / 10;
    if (range < 64)
            range = 64;
    else if (range > 65536)
            range = 65536;

    // set the background to the 20% point lowered
    // by 10% of the range so it's not completely
    // black.  Also check for overrange and don't
    // let a saturated image show up a black
    back = 16L * p20 - range / 10;
    if (p20 >= 4080)        // saturated image?
            back = 16L * 4080 - range;

    *cblack = back;
    *cwhite = back + range;

    return CE_NO_ERROR;
}

int sbig_establish_link (sbig_t *sb, CAMERA_TYPE *type)
{
    EstablishLinkParams in = { .sbigUseOnly = 0 };
    EstablishLinkResults out;
    int e = sb->fun (CC_ESTABLISH_LINK, &in, &out);
    if (e == CE_NO_ERROR)
        *type = out.cameraType;
    return e;
}

typedef struct {
    CAMERA_TYPE type;
    const char *desc;
} devtab_t;

static devtab_t dtab[] = {
  { ST7_CAMERA, "ST-7" },
  { ST8_CAMERA, "ST-8" },
  { ST5C_CAMERA, "ST-5C" },
  { TCE_CONTROLLER, "TCE Controller" },
  { ST237_CAMERA, "ST-237" },
  { STK_CAMERA, "STK" },
  { ST9_CAMERA, "ST9" },
  { STV_CAMERA, "STV" },
  { ST10_CAMERA, "ST10" },
  { ST1K_CAMERA, "ST1K" },
  { ST2K_CAMERA, "ST2K" },
  { STL_CAMERA, "STL" },
  { ST402_CAMERA, "ST402" },
  { STX_CAMERA, "STX" },
  { ST4K_CAMERA, "ST4K" },
  { STT_CAMERA, "STT" },
  { STI_CAMERA, "STI" },
  { STF_CAMERA, "STF" },
  { NEXT_CAMERA, "NEXT" },
  { NO_CAMERA, "no camera" },
};

const char *sbig_strcam (CAMERA_TYPE type)
{
    int i, max = sizeof (dtab) / sizeof (dtab[0]);
    for (i = 0; i < max; i++)
        if (dtab[i].type == type)
            return dtab[i].desc;
    return "unknown";
}

/*
 * vi:tabstop=4 shiftwidth=4 expandtab
 */
