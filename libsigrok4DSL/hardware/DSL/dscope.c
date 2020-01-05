/*
 * This file is part of the libsigrok project.
 *
 * Copyright (C) 2013 Bert Vermeulen <bert@biot.com>
 * Copyright (C) 2013 DreamSourceLab <dreamsourcelab@dreamsourcelab.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "libsigrok.h"
#include "libsigrok-internal.h"

#include "dsl.h"
#include "command.h"


enum {
    /** Normal */
    OP_NORMAL = 0,
    /** Internal pattern test mode */
    OP_INTEST = 1,
};

static const char *opmodes_cn[] = {
    "正常",
    "内部测试",
};

static const char *opmodes[] = {
    "Normal",
    "Internal Test",
};

enum {
    BW_FULL = 0,
    BW_20M = 1,
};

static const char *bandwidths_cn[] = {
    "全带宽",
    "20MHz",
};

static const char *bandwidths[] = {
    "Full Bandwidth",
    "20MHz",
};

static const int32_t hwoptions[] = {
    SR_CONF_OPERATION_MODE,
    SR_CONF_BANDWIDTH_LIMIT,
};

static const int32_t sessions_dso[] = {
    SR_CONF_OPERATION_MODE,
    SR_CONF_TIMEBASE,
    SR_CONF_TRIGGER_SLOPE,
    SR_CONF_TRIGGER_SOURCE,
    SR_CONF_TRIGGER_CHANNEL,
    SR_CONF_HORIZ_TRIGGERPOS,
    SR_CONF_TRIGGER_HOLDOFF,
    SR_CONF_TRIGGER_MARGIN,
};

static const int32_t sessions_daq[] = {
    SR_CONF_SAMPLERATE,
    SR_CONF_LIMIT_SAMPLES,
    SR_CONF_OPERATION_MODE,
    SR_CONF_TIMEBASE,
    SR_CONF_TRIGGER_SLOPE,
    SR_CONF_TRIGGER_SOURCE,
    SR_CONF_TRIGGER_CHANNEL,
    SR_CONF_HORIZ_TRIGGERPOS,
    SR_CONF_TRIGGER_HOLDOFF,
    SR_CONF_TRIGGER_MARGIN,
};

static const uint8_t zero_base_addr = 0x40;
static const uint8_t zero_big_addr = 0x20;

SR_PRIV struct sr_dev_driver DSCope_driver_info;
static struct sr_dev_driver *di = &DSCope_driver_info;

static const char ** get_opmodes(struct DSL_context *devc)
{
    if (devc->language == LANGUAGE_CN)
        return opmodes_cn;
    else
        return opmodes;
}

static const char ** get_bandwidths(struct DSL_context *devc)
{
    if (devc->language == LANGUAGE_CN)
        return bandwidths_cn;
    else
        return bandwidths;
}

static uint16_t get_default_preoff(const struct sr_dev_inst *sdi, const struct sr_channel* ch)
{
    int i;
    struct DSL_context *devc = sdi->priv;
    for (i = 0; vga_defaults[i].id; i++) {
        if (vga_defaults[i].id == devc->profile->dev_caps.vga_id &&
            vga_defaults[i].key == ch->vdiv) {
            if (ch->index == 1)
                return vga_defaults[i].preoff_comp;
            else
                return vga_defaults[i].preoff;
        }
    }

    return 0;
}

static struct DSL_context *DSCope_dev_new(const struct DSL_profile *prof)
{
    struct DSL_context *devc;
    unsigned int i;

    if (!(devc = g_try_malloc(sizeof(struct DSL_context)))) {
        sr_err("Device context malloc failed.");
		return NULL;
	}

    for (i = 0; i < ARRAY_SIZE(channel_modes); i++)
        assert(channel_modes[i].id == i);

    devc->channel = NULL;
    devc->profile = prof;
    devc->fw_updated = 0;
    devc->cur_samplerate = devc->profile->dev_caps.default_samplerate;
    devc->limit_samples = devc->profile->dev_caps.default_samplelimit;
    devc->clock_type = FALSE;
    devc->clock_edge = FALSE;
    devc->instant = FALSE;
    devc->op_mode = OP_NORMAL;
    devc->test_mode = SR_TEST_NONE;
    devc->stream = FALSE;
    devc->ch_mode = devc->profile->dev_caps.default_channelmode;
    devc->th_level = SR_TH_3V3;
    devc->filter = SR_FILTER_NONE;
    devc->timebase = 10000;
    devc->trigger_slope = DSO_TRIGGER_RISING;
    devc->trigger_source = DSO_TRIGGER_AUTO;
    devc->trigger_holdoff = 0;
    devc->trigger_hpos = 0x0;
    devc->trigger_hrate = 0;
    devc->zero = FALSE;
    devc->tune = FALSE;
    devc->data_lock = FALSE;
    devc->cali = FALSE;
    devc->trigger_margin = 8;
    devc->trigger_channel = 0;
    devc->rle_mode = FALSE;
    devc->status = DSL_FINISH;
    devc->bw_limit = BW_FULL;

    dsl_adjust_samplerate(devc);
	return devc;
}

static int dev_clear(void)
{
	return std_dev_clear(di, NULL);
}

static int init(struct sr_context *sr_ctx)
{
    return std_hw_init(sr_ctx, di, LOG_PREFIX);
}

static GSList *scan(GSList *options)
{
	struct drv_context *drvc;
    struct DSL_context *devc;
    struct sr_dev_inst *sdi;
    struct sr_usb_dev_inst *usb;
    struct sr_config *src;
    const struct DSL_profile *prof;
	GSList *l, *devices, *conn_devices;
	struct libusb_device_descriptor des;
	libusb_device **devlist;
    int devcnt, ret, i, j;
	const char *conn;

	drvc = di->priv;

	conn = NULL;
	for (l = options; l; l = l->next) {
		src = l->data;
		switch (src->key) {
        case SR_CONF_CONN:
            conn = g_variant_get_string(src->data, NULL);
			break;
		}
	}
	if (conn)
        conn_devices = sr_usb_find(drvc->sr_ctx->libusb_ctx, conn);
	else
		conn_devices = NULL;

    /* Find all DSCope compatible devices and upload firmware to them. */
	devices = NULL;
    libusb_get_device_list(drvc->sr_ctx->libusb_ctx, &devlist);
	for (i = 0; devlist[i]; i++) {
		if (conn) {
			usb = NULL;
			for (l = conn_devices; l; l = l->next) {
				usb = l->data;
				if (usb->bus == libusb_get_bus_number(devlist[i])
					&& usb->address == libusb_get_device_address(devlist[i]))
					break;
			}
			if (!l)
				/* This device matched none of the ones that
				 * matched the conn specification. */
				continue;
		}

		if ((ret = libusb_get_device_descriptor( devlist[i], &des)) != 0) {
            sr_warn("Failed to get device descriptor: %s.",
				libusb_error_name(ret));
			continue;
		}

		prof = NULL;
        for (j = 0; supported_DSCope[j].vid; j++) {
            if (des.idVendor == supported_DSCope[j].vid &&
                des.idProduct == supported_DSCope[j].pid) {
                prof = &supported_DSCope[j];
			}
		}

		/* Skip if the device was not found. */
		if (!prof)
			continue;

		devcnt = g_slist_length(drvc->instances);
        devc = DSCope_dev_new(prof);
        if (!devc)
            return NULL;
        sdi = sr_dev_inst_new(channel_modes[devc->ch_mode].mode, devcnt, SR_ST_INITIALIZING,
			prof->vendor, prof->model, prof->model_version);
        if (!sdi) {
            g_free(devc);
            return NULL;
        }
        sdi->priv = devc;
		sdi->driver = di;

        drvc->instances = g_slist_append(drvc->instances, sdi);
        //devices = g_slist_append(devices, sdi);

        /* Fill in probelist according to this device's profile. */
        if (dsl_setup_probes(sdi, channel_modes[devc->ch_mode].num) != SR_OK)
            return NULL;

		if (dsl_check_conf_profile(devlist[i])) {
			/* Already has the firmware, so fix the new address. */
            sr_dbg("Found an DSCope device.");
            sdi->status = SR_ST_INACTIVE;
            sdi->inst_type = SR_INST_USB;
            sdi->conn = sr_usb_dev_inst_new(libusb_get_bus_number(devlist[i]),
					libusb_get_device_address(devlist[i]), NULL);
            /* only report device after firmware is ready */
            devices = g_slist_append(devices, sdi);
		} else {
            char *firmware;
            if (!(firmware = g_try_malloc(strlen(DS_RES_PATH)+strlen(prof->firmware)+1))) {
                sr_err("Firmware path malloc error!");
                return NULL;
            }
            strcpy(firmware, DS_RES_PATH);
            strcat(firmware, prof->firmware);
            if (ezusb_upload_firmware(devlist[i], USB_CONFIGURATION,
                firmware) == SR_OK)
				/* Store when this device's FW was updated. */
				devc->fw_updated = g_get_monotonic_time();
			else
                sr_err("Firmware upload failed for "
				       "device %d.", devcnt);
            g_free(firmware);
            sdi->inst_type = SR_INST_USB;
            sdi->conn = sr_usb_dev_inst_new (libusb_get_bus_number(devlist[i]),
					0xff, NULL);
		}
	}
	libusb_free_device_list(devlist, 1);
    g_slist_free_full(conn_devices, (GDestroyNotify)sr_usb_dev_inst_free);

	return devices;
}

static GSList *dev_list(void)
{
	return ((struct drv_context *)(di->priv))->instances;
}

static const GSList *dev_mode_list(const struct sr_dev_inst *sdi)
{
    return dsl_mode_list(sdi);
}

static uint64_t dso_vga(const struct sr_channel* ch)
{
    int i;
    for (i = 0; ch->vga_ptr && (ch->vga_ptr+i)->id; i++) {
        if ((ch->vga_ptr+i)->key == ch->vdiv)
            return (ch->vga_ptr+i)->vgain;
    }

    return 0;
}

static uint64_t dso_preoff(const struct sr_channel* ch)
{
    int i;
    for (i = 0; ch->vga_ptr && (ch->vga_ptr+i)->id; i++) {
        if ((ch->vga_ptr+i)->key == ch->vdiv)
            return (ch->vga_ptr+i)->preoff;
    }
    return 0;
}

static uint64_t dso_offset(const struct sr_dev_inst *sdi, const struct sr_channel* ch)
{
    uint64_t pwm_off = 0;
    int offset_coarse, offset_fine;
    int trans_coarse, trans_fine;
    struct DSL_context *devc = sdi->priv;
    const double offset_mid = (1 << (ch->bits - 1));
    const double offset_max = ((1 << ch->bits) - 1.0);
    const uint64_t offset = devc->zero ? ch->zero_offset : ch->hw_offset;
    double comb_off = 2.0 / (pow(10, 24.0*ch->comb_comp/20/4096) - 1);
//    const double comb_compensate = ((devc->profile->dev_caps.feature_caps & CAPS_FEATURE_HMCAD1511) &&
//                                    (dsl_en_ch_num(sdi) == 1))? (offset - offset_mid) / comb_off : 0;
    const double comb_compensate = ((ch->comb_comp != 0) && (dsl_en_ch_num(sdi) == 1)) ? (offset - offset_mid) / comb_off : 0;
    if (devc->profile->dev_caps.feature_caps & CAPS_FEATURE_PREOFF) {
        trans_coarse = (ch->vpos_trans & 0xFF00) >> 8;
        trans_fine = (ch->vpos_trans & 0x00FF);
        const double voltage = (offset_mid - offset) / offset_max * ch->vdiv * DS_CONF_DSO_VDIVS;
        if (ch->vdiv < 500) {
            offset_coarse = floor(-voltage*DSCOPE_TRANS_CMULTI/trans_coarse + 0.5);
            offset_fine = floor((voltage + offset_coarse*trans_coarse/DSCOPE_TRANS_CMULTI)*1000.0/trans_fine + 0.5);
        } else {
            offset_coarse = floor(-voltage/trans_coarse + 0.5);
            offset_fine = floor((voltage + offset_coarse*trans_coarse)*DSCOPE_TRANS_FMULTI/trans_fine + 0.5);
        }
    } else {
        pwm_off = (offset + comb_compensate) / offset_max * ch->vpos_trans;
    }

    const uint64_t preoff = dso_preoff(ch);
    if (devc->profile->dev_caps.feature_caps & CAPS_FEATURE_PREOFF)
        return (offset << 32) +
               ((offset_coarse + DSCOPE_CONSTANT_BIAS + (preoff>>10)) << 16) + offset_fine +
               (preoff & 0x03ff);
    else
        return (offset << 32) +
               pwm_off + preoff;
}

static uint64_t dso_cmd_gen(const struct sr_dev_inst *sdi, struct sr_channel* ch, int id)
{
    struct DSL_context *devc;
    uint64_t cmd = 0;
    uint64_t offset;
    GSList *l;
    const int ch_bit = 7;
    devc = sdi->priv;

    switch (id) {
    case SR_CONF_PROBE_EN:
    case SR_CONF_PROBE_COUPLING:
        if (devc->zero || sdi->mode == ANALOG || dsl_en_ch_num(sdi) == 2) {
            cmd += 0x0E00;
            //cmd += 0x000;
        } else if (dsl_en_ch_num(sdi) == 1) {
            if (((ch->index == 0) && ch->enabled) || ((ch->index == 1) && !ch->enabled))
                cmd += 0x1600;
            else if (((ch->index == 1) && ch->enabled) || ((ch->index == 0) && !ch->enabled))
                cmd += 0x1A00;
        } else {
            return 0x0;
        }

        cmd += ch->index << ch_bit;
        if (devc->zero || ch->coupling == SR_DC_COUPLING)
            cmd += 0x100;
        else if (ch->coupling == SR_GND_COUPLING)
            cmd &= 0xFFFFFDFF;
        break;
    case SR_CONF_PROBE_VDIV:
    case SR_CONF_TIMEBASE:
        cmd += 0x8;
        cmd += ch->index << ch_bit;
        //  --VGAIN
        uint64_t vgain = dso_vga(ch);
//        if ((devc->profile->dev_caps.feature_caps & CAPS_FEATURE_HMCAD1511) &&
//            (dsl_en_ch_num(sdi) == 1))
        if ((ch->comb_comp != 0) && (dsl_en_ch_num(sdi) == 1))
            vgain += (uint64_t)(ch->comb_comp) << 8;
        cmd += vgain;
        break;
    case SR_CONF_PROBE_OFFSET:
        cmd += 0x10;
        cmd += ch->index << ch_bit;
        ch->hw_offset = ch->offset;
        offset = dso_offset(sdi, ch);
        cmd += (offset << 8);
        break;
    case SR_CONF_SAMPLERATE:
        cmd += 0x18;
        uint32_t divider = devc->zero ? 0x1 : (uint32_t)ceil(channel_modes[devc->ch_mode].max_samplerate * 1.0 / devc->cur_samplerate / dsl_en_ch_num(sdi));
        cmd += divider << 8;
        break;
    case SR_CONF_HORIZ_TRIGGERPOS:
        cmd += 0x20;
        cmd += devc->trigger_hpos << 8;
        break;
    case SR_CONF_TRIGGER_SLOPE:
        cmd += 0x28;
        cmd += devc->trigger_slope << 8;
        break;
    case SR_CONF_TRIGGER_SOURCE:
        cmd += 0x30;
        cmd += devc->zero ? 0x0 : devc->trigger_source << 8;
        break;
    case SR_CONF_TRIGGER_VALUE:
        cmd += 0x38;
        for (l = sdi->channels; l; l = l->next) {
            struct sr_channel *probe = (struct sr_channel *)l->data;
            cmd += probe->trig_value << (8 * (probe->index + 1));
        }
        break;
    case SR_CONF_TRIGGER_MARGIN:
        cmd += 0x40;
        cmd += ((uint64_t)devc->trigger_margin << 8);
        break;
    case SR_CONF_TRIGGER_HOLDOFF:
        cmd += 0x58;
        cmd += ((uint64_t)devc->trigger_holdoff << 8);
        break;
    case SR_CONF_DSO_SYNC:
        cmd = 0xa5a5a500;
        break;
    default:
        cmd = 0xFFFFFFFF;
    }

    return cmd;
}

static int dso_init(const struct sr_dev_inst *sdi)
{
    int ret;
    GSList *l;

    for(l = sdi->channels; l; l = l->next) {
        struct sr_channel *probe = (struct sr_channel *)l->data;
        ret = dsl_wr_dso(sdi, dso_cmd_gen(sdi, probe, SR_CONF_PROBE_COUPLING));
        if (ret != SR_OK) {
            sr_err("DSO set coupling of channel %d command failed!", probe->index);
            return ret;
        }
        ret = dsl_wr_dso(sdi, dso_cmd_gen(sdi, probe, SR_CONF_PROBE_VDIV));
        if (ret != SR_OK) {
            sr_err("Set VDIV of channel %d command failed!", probe->index);
            return ret;
        }
        ret = dsl_wr_dso(sdi, dso_cmd_gen(sdi, probe, SR_CONF_PROBE_OFFSET));
        if (ret != SR_OK) {
            sr_err("Set OFFSET of channel %d command failed!", probe->index);
            return ret;
        }
    }

    ret = dsl_wr_dso(sdi, dso_cmd_gen(sdi, 0, SR_CONF_SAMPLERATE));
    if (ret != SR_OK) {
        sr_err("Set Sample Rate command failed!");
        return ret;
    }
    ret = dsl_wr_dso(sdi, dso_cmd_gen(sdi, NULL, SR_CONF_HORIZ_TRIGGERPOS));
    if (ret != SR_OK) {
        sr_err("Set Horiz Trigger Position command failed!");
        return ret;
    }
    ret = dsl_wr_dso(sdi, dso_cmd_gen(sdi, NULL, SR_CONF_TRIGGER_HOLDOFF));
    if (ret != SR_OK) {
        sr_err("Set Trigger Holdoff Time command failed!");
        return ret;
    }
    ret = dsl_wr_dso(sdi, dso_cmd_gen(sdi, NULL, SR_CONF_TRIGGER_SLOPE));
    if (ret != SR_OK) {
        sr_err("Set Trigger Slope command failed!");
        return ret;
    }
    ret = dsl_wr_dso(sdi, dso_cmd_gen(sdi, NULL, SR_CONF_TRIGGER_SOURCE));
    if (ret != SR_OK) {
        sr_err("Set Trigger Source command failed!");
        return ret;
    }
    ret = dsl_wr_dso(sdi, dso_cmd_gen(sdi, NULL, SR_CONF_TRIGGER_VALUE));
    if (ret != SR_OK) {
        sr_err("Set Trigger Value command failed!");
        return ret;
    }
    ret = dsl_wr_dso(sdi, dso_cmd_gen(sdi, NULL, SR_CONF_TRIGGER_MARGIN));
    if (ret != SR_OK) {
        sr_err("Set Trigger Margin command failed!");
        return ret;
    }
    return ret;
}

static gboolean dso_load_eep(struct sr_dev_inst *sdi, struct sr_channel *probe, gboolean fpga_done)
{
    struct DSL_context *devc;
    int ret, i;
    uint16_t real_zero_addr;

    devc = sdi->priv;
    struct cmd_zero_info zero_info;
    uint8_t dst_addr = (zero_base_addr +
                        probe->index * (sizeof(struct cmd_zero_info) + sizeof(struct cmd_vga_info)));
    zero_info.zero_addr = dst_addr;
    if (devc->profile->dev_caps.feature_caps & CAPS_FEATURE_SEEP)
        real_zero_addr = zero_info.zero_addr;
    else
        real_zero_addr = (zero_big_addr << 8) + zero_info.zero_addr;
    if ((ret = dsl_rd_nvm(sdi, (unsigned char *)&zero_info, real_zero_addr, sizeof(struct cmd_zero_info))) != SR_OK) {
        return FALSE;
        sr_err("%s: Send Get Zero command failed!", __func__);
    } else {
        if (zero_info.zero_addr == dst_addr) {
            uint8_t* preoff_ptr = &zero_info.zero_addr + 1;
             for (i = 0; probe->vga_ptr && (probe->vga_ptr+i)->id; i++) {
                 (probe->vga_ptr+i)->preoff = (*(preoff_ptr + 2*i+1) << 8) + *(preoff_ptr + 2*i);
             }
             if (i != 0) {
                 probe->comb_diff_top = *(preoff_ptr + 2*i);
                 probe->comb_diff_bom = *(preoff_ptr + 2*i + 1);
                 probe->vpos_trans = *(preoff_ptr + 2*i + 2) + (*(preoff_ptr + 2*i + 3) << 8);
                 probe->comb_comp = *(preoff_ptr + 2*i + 4);
                 if (!fpga_done) {
                     const double slope = (probe->comb_diff_bom - probe->comb_diff_top)/(2.0*255.0);
                     for (i = 0; i < 256; i++) {
                         ret = dsl_wr_reg(sdi, COMB_ADDR + probe->index*2, i);
                         int value = i+i*slope+probe->comb_diff_top*0.5+0.5;
                         value = (value < 0) ? 0 :
                                 (value > 255) ? 255 : value;
                         ret = dsl_wr_reg(sdi, COMB_ADDR + probe->index*2 + 1, value);
                     }
                 }
             }
        } else {
            return FALSE;
        }
    }

    struct cmd_vga_info vga_info;
    vga_info.vga_addr = dst_addr + sizeof(struct cmd_zero_info);
    if (devc ->profile->dev_caps.feature_caps & CAPS_FEATURE_SEEP)
        real_zero_addr = vga_info.vga_addr;
    else
        real_zero_addr = (zero_big_addr << 8) + vga_info.vga_addr;
    if ((ret = dsl_rd_nvm(sdi, (unsigned char *)&vga_info, real_zero_addr, sizeof(struct cmd_vga_info))) != SR_OK) {
        return FALSE;
        sr_err("%s: Send Get Zero command failed!", __func__);
    } else {
        if (vga_info.vga_addr == dst_addr + sizeof(struct cmd_zero_info)) {
            uint16_t* vgain_ptr = &vga_info.vga0;
            for (i = 0; probe->vga_ptr && (probe->vga_ptr+i)->id; i++) {
                (probe->vga_ptr+i)->vgain = *(vgain_ptr + i) << 8;
            }
        } else {
            return FALSE;
        }
    }

    return TRUE;
}

static int config_get(int id, GVariant **data, const struct sr_dev_inst *sdi,
                      const struct sr_channel *ch,
                      const struct sr_channel_group *cg)
{
    unsigned int i;
    int ret;
    struct DSL_context *devc = sdi->priv;

    ret = dsl_config_get(id, data, sdi, ch, cg);
    if (ret != SR_OK) {
        switch (id) {
        case SR_CONF_OPERATION_MODE:
            if (!sdi)
                return SR_ERR;
            *data = g_variant_new_string(get_opmodes(devc)[devc->op_mode]);
            break;
        case SR_CONF_BANDWIDTH_LIMIT:
            if (!sdi)
                return SR_ERR;
            *data = g_variant_new_string(get_bandwidths(devc)[devc->bw_limit]);
            break;
        case SR_CONF_CALI:
            if (!sdi)
                return SR_ERR;
            *data = g_variant_new_boolean(devc->cali);
            break;
        case SR_CONF_TEST:
            if (!sdi)
                return SR_ERR;
            *data = g_variant_new_boolean(FALSE);
            break;
        case SR_CONF_STREAM:
            if (!sdi)
                return SR_ERR;
            *data = g_variant_new_boolean(devc->stream);
            break;
        case SR_CONF_MAX_DSO_SAMPLERATE:
            if (!sdi)
                return SR_ERR;
            *data = g_variant_new_uint64(channel_modes[devc->ch_mode].max_samplerate);
            break;
        case SR_CONF_MAX_DSO_SAMPLELIMITS:
            if (!sdi)
                return SR_ERR;
            *data = g_variant_new_uint64(devc->profile->dev_caps.dso_depth);
            break;
        case SR_CONF_HW_DEPTH:
            if (!sdi)
                return SR_ERR;
            *data = g_variant_new_uint64(devc->profile->dev_caps.hw_depth / channel_modes[devc->ch_mode].unit_bits);
            break;
        case SR_CONF_PROBE_VGAIN:
            if (!sdi || !ch)
                return SR_ERR;
            *data = g_variant_new_uint64(dso_vga(ch)>>8);
            break;
        case SR_CONF_PROBE_COMB_COMP_EN:
            if (!sdi || !ch)
                return SR_ERR;
            *data = g_variant_new_boolean(devc->profile->dev_caps.feature_caps & CAPS_FEATURE_HMCAD1511);
            break;
        case SR_CONF_PROBE_COMB_COMP:
            if (!sdi || !ch)
                return SR_ERR;
            *data = g_variant_new_int16(ch->comb_comp);
            break;
        case SR_CONF_PROBE_VGAIN_DEFAULT:
            if (!sdi || !ch)
                return SR_ERR;
            for (i = 0; vga_defaults[i].id; i++) {
                if (vga_defaults[i].id == devc->profile->dev_caps.vga_id &&
                    vga_defaults[i].key == ch->vdiv) {
                    *data = g_variant_new_uint64(vga_defaults[i].vgain >> 8);
                    break;
                }
            }
            break;
        case SR_CONF_PROBE_VGAIN_RANGE:
            if (!sdi)
                return SR_ERR;
            uint16_t vgain_default = 0;
            for (i = 0; vga_defaults[i].id; i++) {
                if (vga_defaults[i].id == devc->profile->dev_caps.vga_id &&
                    vga_defaults[i].key == ch->vdiv) {
                    vgain_default = vga_defaults[i].vgain;
                    break;
                }
            }
            vgain_default = (vgain_default>>8) & 0x0FFF;
            *data = g_variant_new_uint16(min(CALI_VGAIN_RANGE, vgain_default*2));
            break;
        case SR_CONF_PROBE_PREOFF:
            if (!sdi || !ch)
                return SR_ERR;
            uint16_t preoff = dso_preoff(ch);
            uint16_t preoff_default = get_default_preoff(sdi, ch);
            if (devc->profile->dev_caps.feature_caps & CAPS_FEATURE_PREOFF) {
                int preoff_skew_coarse = (preoff >> 10) - (preoff_default >> 10);
                int preoff_skew_fine = (preoff & 0x03ff) - (preoff_default & 0x03ff);
                double trans_coarse = (ch->vdiv < 500) ? (ch->vpos_trans >> 8)/DSCOPE_TRANS_CMULTI : (ch->vpos_trans >> 8);
                double trans_fine = (ch->vdiv < 500) ? (ch->vpos_trans & 0x00ff) / 1000.0 : (ch->vpos_trans & 0x00ff) / DSCOPE_TRANS_FMULTI;
                double preoff_rate = (preoff_skew_coarse*trans_coarse - preoff_skew_fine*trans_fine) / ch->vdiv;
                preoff = (preoff_rate * 0.5 + 0.5) * devc->profile->dev_caps.default_pwmmargin;
            }
            *data = g_variant_new_uint16(preoff);
            break;
        case SR_CONF_PROBE_PREOFF_DEFAULT:
            if (!sdi || !ch)
                return SR_ERR;
            *data = g_variant_new_uint16(get_default_preoff(sdi, ch));
            break;
        case SR_CONF_PROBE_PREOFF_MARGIN:
            if (!sdi)
                return SR_ERR;
            *data = g_variant_new_uint16(devc->profile->dev_caps.default_pwmmargin);
            break;
        case SR_CONF_PROBE_MAP_DEFAULT:
            if (!sdi || !ch)
                return SR_ERR;
            *data = g_variant_new_boolean(ch->map_default);
            break;
        case SR_CONF_PROBE_MAP_UNIT:
            if (!sdi || !ch)
                return SR_ERR;
            *data = g_variant_new_string(ch->map_unit);
            break;
        case SR_CONF_PROBE_MAP_MIN:
            if (!sdi || !ch)
                return SR_ERR;
            *data = g_variant_new_double(ch->map_min);
            break;
        case SR_CONF_PROBE_MAP_MAX:
            if (!sdi || !ch)
                return SR_ERR;
            *data = g_variant_new_double(ch->map_max);
            break;
        case SR_CONF_VLD_CH_NUM:
            if (!sdi)
                return SR_ERR;
            *data = g_variant_new_int16(channel_modes[devc->ch_mode].vld_num);
            break;
        default:
            return SR_ERR_NA;
        }
    }

    return SR_OK;
}

static int config_set(int id, GVariant *data, struct sr_dev_inst *sdi,
                      struct sr_channel *ch,
                      struct sr_channel_group *cg )
{
    struct DSL_context *devc;
    const char *stropt;
    int ret, num_probes;
    struct sr_usb_dev_inst *usb;
    struct libusb_device_handle *hdl;
    struct ctl_wr_cmd wr_cmd;
    unsigned int i;
    GSList *l;

    (void)cg;

    if (sdi->status != SR_ST_ACTIVE) {
        return SR_ERR;
    }

	devc = sdi->priv;
    usb = sdi->conn;
    hdl = usb->devhdl;

    ret = dsl_config_set(id, data, sdi, ch, cg);
    if (ret == SR_OK)
        return ret;

    ret = SR_OK;
    if (id == SR_CONF_CLOCK_TYPE) {
        devc->clock_type = g_variant_get_boolean(data);
    } else if (id == SR_CONF_CLOCK_EDGE) {
        devc->clock_edge = g_variant_get_boolean(data);
    } else if (id == SR_CONF_LIMIT_SAMPLES) {
        devc->limit_samples = g_variant_get_uint64(data);
    } else if (id == SR_CONF_PROBE_VDIV) {
        ch->vdiv = g_variant_get_uint64(data);
        ret = dsl_wr_dso(sdi, dso_cmd_gen(sdi, ch, SR_CONF_PROBE_VDIV));
        if (ret == SR_OK)
            sr_dbg("%s: setting VDIV of channel %d to %d mv",
                __func__, ch->index, ch->vdiv);
        else
            sr_dbg("%s: setting VDIV of channel %d to %d mv failed",
                __func__, ch->index, ch->vdiv);
    } else if (id == SR_CONF_PROBE_FACTOR) {
        ch->vfactor = g_variant_get_uint64(data);
        sr_dbg("%s: setting Factor of channel %d to %d", __func__,
               ch->index, ch->vfactor);
    } else if (id == SR_CONF_TIMEBASE) {
        devc->timebase = g_variant_get_uint64(data);
    } else if (id == SR_CONF_PROBE_COUPLING) {
        ch->coupling = g_variant_get_byte(data);
        if (ch->coupling == SR_GND_COUPLING)
            ch->coupling = SR_DC_COUPLING;
        ret = dsl_wr_dso(sdi, dso_cmd_gen(sdi, ch, SR_CONF_PROBE_COUPLING));
        if (ret == SR_OK)
            sr_dbg("%s: setting AC COUPLING of channel %d to %d",
                __func__, ch->index, ch->coupling);
        else
            sr_dbg("%s: setting AC COUPLING of channel %d to %d failed",
                __func__, ch->index, ch->coupling);
    } else if (id == SR_CONF_TRIGGER_SLOPE) {
        devc->trigger_slope = g_variant_get_byte(data);
        if (sdi->mode == DSO) {
            ret = dsl_wr_dso(sdi, dso_cmd_gen(sdi, NULL, SR_CONF_TRIGGER_SLOPE));
        }
        if (ret == SR_OK)
            sr_dbg("%s: setting DSO Trigger Slope to %d",
                __func__, devc->trigger_slope);
        else
            sr_dbg("%s: setting DSO Trigger Slope to %d failed",
                __func__, devc->trigger_slope);
    } else if (id == SR_CONF_TRIGGER_VALUE) {
        ch->trig_value = g_variant_get_byte(data);
        if (sdi->mode == DSO) {
            ret = dsl_wr_dso(sdi, dso_cmd_gen(sdi, ch, SR_CONF_TRIGGER_VALUE));
        }
        if (ret == SR_OK)
            sr_dbg("%s: setting channel %d Trigger Value to %d",
                __func__, ch->index, ch->trig_value);
        else
            sr_dbg("%s: setting DSO Trigger Value to %d failed",
                __func__, ch->index, ch->trig_value);
    } else if (id == SR_CONF_HORIZ_TRIGGERPOS) {
        if (sdi->mode == DSO) {
            devc->trigger_hrate = g_variant_get_byte(data);
            //devc->trigger_hpos = devc->trigger_hrate * dsl_en_ch_num(sdi) * devc->limit_samples / 200.0;
            /*
             * devc->trigger_hpos should be updated before each acquisition
             * because the samplelimits may changed
             */
            devc->trigger_hpos = devc->trigger_hrate * dsl_en_ch_num(sdi) * devc->limit_samples / 200.0;
            if ((ret = dsl_wr_dso(sdi, dso_cmd_gen(sdi, NULL, SR_CONF_HORIZ_TRIGGERPOS))) == SR_OK)
                sr_dbg("%s: setting DSO Horiz Trigger Position to %d",
                    __func__, devc->trigger_hpos);
            else
                sr_dbg("%s: setting DSO Horiz Trigger Position to %d failed",
                    __func__, devc->trigger_hpos);
        } else {
            devc->trigger_hpos = g_variant_get_byte(data) * devc->limit_samples / 100.0;
        }
    } else if (id == SR_CONF_TRIGGER_HOLDOFF) {
        devc->trigger_holdoff = g_variant_get_uint64(data);
        if (sdi->mode == DSO) {
            ret = dsl_wr_dso(sdi, dso_cmd_gen(sdi, NULL, SR_CONF_TRIGGER_HOLDOFF));
        }
        if (ret == SR_OK)
            sr_dbg("%s: setting Trigger Holdoff Time to %d",
                __func__, devc->trigger_holdoff);
        else
            sr_dbg("%s: setting Trigger Holdoff Time to %d failed",
                __func__, devc->trigger_holdoff);
    } else if (id == SR_CONF_TRIGGER_MARGIN) {
        devc->trigger_margin = g_variant_get_byte(data);
        if (sdi->mode == DSO) {
            ret = dsl_wr_dso(sdi, dso_cmd_gen(sdi, NULL, SR_CONF_TRIGGER_MARGIN));
        }
        if (ret == SR_OK)
            sr_dbg("%s: setting Trigger Margin to %d",
                __func__, devc->trigger_margin);
        else
            sr_dbg("%s: setting Trigger Margin to %d failed",
                __func__, devc->trigger_margin);
    } else if (id == SR_CONF_SAMPLERATE) {
        devc->cur_samplerate = g_variant_get_uint64(data);
        if(sdi->mode == DSO) {
            ret = dsl_wr_dso(sdi, dso_cmd_gen(sdi, 0, SR_CONF_SAMPLERATE));
        }
    } else if (id == SR_CONF_INSTANT) {
        devc->instant = g_variant_get_boolean(data);
        if (sdi->mode == DSO && dsl_en_ch_num(sdi) != 0) {
            if (devc->instant)
                devc->limit_samples = devc->profile->dev_caps.hw_depth / channel_modes[devc->ch_mode].unit_bits /  dsl_en_ch_num(sdi);
            else
                devc->limit_samples = devc->profile->dev_caps.dso_depth / dsl_en_ch_num(sdi);
        }
    } else if (id == SR_CONF_DEVICE_MODE) {
        sdi->mode = g_variant_get_int16(data);
        num_probes = 0;
        if (sdi->mode == DSO) {
            ret = dsl_wr_dso(sdi, dso_cmd_gen(sdi, NULL, SR_CONF_DSO_SYNC));
            if (ret != SR_OK)
                sr_dbg("%s: DSO configuration sync failed", __func__);
            ret = dsl_wr_dso(sdi, dso_cmd_gen(sdi, sdi->channels->data, SR_CONF_PROBE_VDIV));
            if (ret == SR_OK)
                sr_dbg("%s: Initial setting for DSO mode", __func__);
            else
                sr_dbg("%s: Initial setting for DSO mode failed", __func__);
            devc->op_mode = OP_NORMAL;
            devc->test_mode = SR_TEST_NONE;
            devc->instant = FALSE;
            for (i = 0; i < ARRAY_SIZE(channel_modes); i++) {
                if (channel_modes[i].mode == DSO &&
                    devc->profile->dev_caps.channels & (1 << i)) {
                    devc->ch_mode = channel_modes[i].id;
                    num_probes = channel_modes[i].num;
                    devc->stream = channel_modes[i].stream;
                    devc->cur_samplerate = channel_modes[i].max_samplerate / num_probes;
                    break;
                }
            }
            devc->limit_samples = devc->profile->dev_caps.dso_depth / num_probes;
        } else if (sdi->mode == ANALOG) {
            ret = dsl_wr_dso(sdi, dso_cmd_gen(sdi, sdi->channels->data, SR_CONF_PROBE_VDIV));
            if (ret == SR_OK)
                sr_dbg("%s: Initial setting for DSO mode", __func__);
            else
                sr_dbg("%s: Initial setting for DSO mode failed", __func__);
            devc->op_mode = OP_NORMAL;
            devc->test_mode = SR_TEST_NONE;
            devc->instant = TRUE;
            for (i = 0; i < ARRAY_SIZE(channel_modes); i++) {
                if (channel_modes[i].mode == ANALOG &&
                    devc->profile->dev_caps.channels & (1 << i)) {
                    devc->ch_mode = channel_modes[i].id;
                    num_probes = channel_modes[i].num;
                    devc->stream = channel_modes[i].stream;
                    devc->cur_samplerate = channel_modes[i].max_samplerate;
                    break;
                }
            }
            devc->limit_samples = devc->cur_samplerate;
        }
        assert(num_probes != 0);
        dsl_adjust_probes(sdi, num_probes);
        dsl_adjust_samplerate(devc);
        sr_dbg("%s: setting mode to %d", __func__, sdi->mode);
    } else if (id == SR_CONF_OPERATION_MODE) {
        stropt = g_variant_get_string(data, NULL);
        if (!strcmp(stropt, get_opmodes(devc)[OP_NORMAL])) {
            devc->op_mode = OP_NORMAL;
            devc->test_mode = SR_TEST_NONE;
        } else if (!strcmp(stropt, get_opmodes(devc)[OP_INTEST])) {
            devc->op_mode = OP_INTEST;
            devc->test_mode = SR_TEST_INTERNAL;
        } else {
            ret = SR_ERR;
        }
        sr_dbg("%s: setting pattern to %d",
            __func__, devc->op_mode);
    } else if (id == SR_CONF_BANDWIDTH_LIMIT) {
        stropt = g_variant_get_string(data, NULL);
        if (!strcmp(stropt, get_bandwidths(devc)[BW_FULL])) {
            devc->bw_limit = BW_FULL;
            dsl_wr_reg(sdi, CTR0_ADDR, bmBW20M_CLR);
        } else if (!strcmp(stropt, get_bandwidths(devc)[BW_20M])) {
            devc->bw_limit = BW_20M;
            dsl_wr_reg(sdi, CTR0_ADDR, bmBW20M_SET);
        } else {
            ret = SR_ERR;
        }
        sr_dbg("%s: setting bandwidth limit to %d",
            __func__, devc->bw_limit);
    } else if (id == SR_CONF_PROBE_EN) {
        ch->enabled = g_variant_get_boolean(data);

        if (sdi->mode == DSO) {
            if (devc->status == DSL_DATA &&
                devc->profile->dev_caps.feature_caps & CAPS_FEATURE_HMCAD1511) {
                if (dsl_en_ch_num(sdi) == 2) {
                    dsl_config_adc(sdi, adc_dual_ch03);
                } else if (dsl_en_ch_num(sdi) == 1) {
                    for (l = sdi->channels; l; l = l->next) {
                        struct sr_channel *probe = (struct sr_channel *)l->data;
                        if (probe->enabled && probe->index == 0) {
                            dsl_config_adc(sdi, adc_single_ch0);
                            break;
                        } else if (probe->enabled && probe->index == 1) {
                            dsl_config_adc(sdi, adc_single_ch3);
                            break;
                        }
                    }
                }
            }
            ret = dsl_wr_dso(sdi, dso_cmd_gen(sdi, ch, SR_CONF_PROBE_EN));
            if (ch->index == 0) {
                wr_cmd.header.dest = DSL_CTL_DSO_EN0;
                wr_cmd.data[0] = ch->enabled ? bmCH_CH0 : (uint8_t)~bmCH_CH0;
            } else {
                wr_cmd.header.dest = DSL_CTL_DSO_EN1;
                wr_cmd.data[0] = ch->enabled ? bmCH_CH1 : (uint8_t)~bmCH_CH1;
            }
            wr_cmd.header.size = 1;
            ret = command_ctl_wr(hdl, wr_cmd);
            if (dsl_en_ch_num(sdi) != 0) {
                ret = dsl_wr_dso(sdi, dso_cmd_gen(sdi, 0, SR_CONF_SAMPLERATE));
                devc->limit_samples = devc->profile->dev_caps.dso_depth / dsl_en_ch_num(sdi);
            }
        } else if (sdi->mode == ANALOG) {
            ret = dsl_wr_dso(sdi, dso_cmd_gen(sdi, ch, SR_CONF_PROBE_EN));
            if (ch->index == 0) {
                wr_cmd.header.dest = DSL_CTL_DSO_EN0;
                wr_cmd.data[0] = bmCH_CH0;
            } else {
                wr_cmd.header.dest = DSL_CTL_DSO_EN1;
                wr_cmd.data[0] = bmCH_CH1;
            }
            wr_cmd.header.size = 1;
            ret = command_ctl_wr(hdl, wr_cmd);
        }
        if (ret == SR_OK)
            sr_dbg("%s: setting ENABLE of channel %d to %d",
                __func__, ch->index, ch->enabled);
        else
            sr_dbg("%s: setting ENABLE of channel %d to %d failed",
                __func__, ch->index, ch->enabled);
    } else if (id == SR_CONF_PROBE_OFFSET) {
        ch->offset = g_variant_get_uint16(data);
        if (devc->status != DSL_FINISH)
            ret = dsl_wr_dso(sdi, dso_cmd_gen(sdi, ch, SR_CONF_PROBE_OFFSET));
        else
            ret = SR_OK;
        if (ret == SR_OK)
            sr_dbg("%s: setting OFFSET of channel %d to %d",
                __func__, ch->index, ch->offset);
        else
            sr_dbg("%s: setting OFFSET of channel %d to %d failed",
                __func__, ch->index, ch->offset);
    } else if (id == SR_CONF_TRIGGER_SOURCE) {
        devc->trigger_source = (devc->trigger_source & 0xf0) + (g_variant_get_byte(data) & 0x0f);
        ret = dsl_wr_dso(sdi, dso_cmd_gen(sdi, NULL, SR_CONF_TRIGGER_SOURCE));
        if (ret == SR_OK)
            sr_dbg("%s: setting DSO Trigger Source to %d",
                __func__, devc->trigger_source);
        else
            sr_dbg("%s: setting DSO Trigger Source to %d failed",
                __func__, devc->trigger_source);
    } else if (id == SR_CONF_TRIGGER_CHANNEL) {
        devc->trigger_source = (g_variant_get_byte(data) << 4) + (devc->trigger_source & 0x0f);
        ret = dsl_wr_dso(sdi, dso_cmd_gen(sdi, NULL, SR_CONF_TRIGGER_SOURCE));
        if (ret == SR_OK)
            sr_dbg("%s: setting DSO Trigger Source to %d",
                __func__, devc->trigger_source);
        else
            sr_dbg("%s: setting DSO Trigger Source to %d failed",
                __func__, devc->trigger_source);
    } else if (id == SR_CONF_ZERO) {
        devc->zero = g_variant_get_boolean(data);
        if (devc->zero) {
            devc->zero_stage = -1;
            devc->zero_pcnt = 0;
            GSList *l;
            unsigned int i, j;
            for(l = sdi->channels; l; l = l->next) {
                struct sr_channel *probe = (struct sr_channel *)l->data;
                probe->vpos_trans = devc->profile->dev_caps.default_pwmtrans;
                probe->comb_comp = devc->profile->dev_caps.default_comb_comp;
                if (probe->vga_ptr != NULL) {
                    for (i = 0; devc->profile->dev_caps.vdivs[i]; i++) {
                        for (j = 0; j < ARRAY_SIZE(vga_defaults); j++) {
                            if (vga_defaults[j].id == devc->profile->dev_caps.vga_id &&
                                vga_defaults[j].key == devc->profile->dev_caps.vdivs[i]) {
                                (probe->vga_ptr+i)->id = vga_defaults[j].id;
                                (probe->vga_ptr+i)->key = vga_defaults[j].key;
                                //(probe->vga_ptr+i)->vgain = vga_defaults[j].vgain;
                                (probe->vga_ptr+i)->preoff = vga_defaults[j].preoff;
                                break;
                            }
                        }
                    }
                }
            }  
        }
    } else if (id == SR_CONF_ZERO_DEFAULT) {
        unsigned int i, j;
        for(l = sdi->channels; l; l = l->next) {
            struct sr_channel *probe = (struct sr_channel *)l->data;
            probe->vpos_trans = devc->profile->dev_caps.default_pwmtrans;
            probe->comb_comp = devc->profile->dev_caps.default_comb_comp;
            if (probe->vga_ptr != NULL) {
                for (i = 0; devc->profile->dev_caps.vdivs[i]; i++) {
                    for (j = 0; j < ARRAY_SIZE(vga_defaults); j++) {
                        if (vga_defaults[j].id == devc->profile->dev_caps.vga_id &&
                            vga_defaults[j].key == devc->profile->dev_caps.vdivs[i]) {
                            (probe->vga_ptr+i)->id = vga_defaults[j].id;
                            (probe->vga_ptr+i)->key = vga_defaults[j].key;
                            (probe->vga_ptr+i)->vgain = vga_defaults[j].vgain;
                            (probe->vga_ptr+i)->preoff = vga_defaults[j].preoff;
                            break;
                        }
                    }
                }
            }
        }
    } else if (id == SR_CONF_CALI) {
        devc->cali = g_variant_get_boolean(data);
    } else if (id == SR_CONF_ZERO_LOAD) {
        GSList *l;
        for(l = sdi->channels; l; l = l->next) {
            struct sr_channel *probe = (struct sr_channel *)l->data;
            if (!dso_load_eep(sdi, probe, FALSE)) {
                config_set(SR_CONF_ZERO, g_variant_new_boolean(TRUE), sdi, NULL, NULL);
                sr_info("Zero have not been setted!");
                break;
            }
        }
    } else if (id == SR_CONF_ZERO_SET) {
        GSList *l;
        struct cmd_zero_info zero_info;
        struct cmd_vga_info vga_info;
        for(l = sdi->channels; l; l = l->next) {
            struct sr_channel *probe = (struct sr_channel *)l->data;
            zero_info.zero_addr = zero_base_addr +
                                  probe->index * (sizeof(struct cmd_zero_info) + sizeof(struct cmd_vga_info));
            int i;
            uint16_t real_zero_addr;
            uint8_t *preoff_ptr = &zero_info.zero_addr + 1;
            for (i = 0; probe->vga_ptr && (probe->vga_ptr+i)->id; i++) {
                *(preoff_ptr+2*i) = (probe->vga_ptr+i)->preoff & 0x00ff;
                *(preoff_ptr+2*i+1) = (probe->vga_ptr+i)->preoff >> 8;
            }
            if (i != 0) {
                *(preoff_ptr+2*i) = probe->comb_diff_top;
                *(preoff_ptr+2*i+1) = probe->comb_diff_bom;
                *(preoff_ptr+2*i+2) = (probe->vpos_trans&0x00FF);
                *(preoff_ptr+2*i+3) = (probe->vpos_trans>>8);
                *(preoff_ptr+2*i+4) = probe->comb_comp;

                vga_info.vga_addr = zero_info.zero_addr + sizeof(struct cmd_zero_info);
                uint16_t *vgain_ptr = &vga_info.vga0;
                for (i=0; probe->vga_ptr && (probe->vga_ptr+i)->id; i++){
                    *(vgain_ptr+i) = (probe->vga_ptr+i)->vgain >> 8;
                }
                ret = dsl_wr_reg(sdi, CTR0_ADDR, bmEEWP);
                if (ret == SR_OK) {
                    if (devc->profile->dev_caps.feature_caps & CAPS_FEATURE_SEEP)
                        real_zero_addr = zero_info.zero_addr;
                    else
                        real_zero_addr = (zero_big_addr << 8) + zero_info.zero_addr;
                    ret = dsl_wr_nvm(sdi, (unsigned char *)&zero_info, real_zero_addr, sizeof(struct cmd_zero_info));
                }
                if (ret == SR_OK) {
                    if (devc->profile->dev_caps.feature_caps & CAPS_FEATURE_SEEP)
                        real_zero_addr = vga_info.vga_addr;
                    else
                        real_zero_addr = (zero_big_addr << 8) + vga_info.vga_addr;
                    ret = dsl_wr_nvm(sdi, (unsigned char *)&vga_info, real_zero_addr, sizeof(struct cmd_vga_info));
                }
                ret = dsl_wr_reg(sdi, CTR0_ADDR, bmNONE);

                if (!(devc->profile->dev_caps.feature_caps & CAPS_FEATURE_HMCAD1511)) {
                    const double slope = (probe->comb_diff_bom - probe->comb_diff_top)/(2.0*255.0);
                    for (i = 0; i < 256; i++) {
                        ret = dsl_wr_reg(sdi, COMB_ADDR + probe->index*2, i);
                        int value = i+i*slope+probe->comb_diff_top*0.5+0.5;
                        value = (value < 0) ? 0 :
                                (value > 255) ? 255 : value;
                        ret = dsl_wr_reg(sdi, COMB_ADDR + probe->index*2 + 1, value);
                    }
                }
            }
        }
    } else if (id == SR_CONF_VOCM) {
        const uint8_t vocm = g_variant_get_byte(data);
        ret = dsl_wr_reg(sdi, COMB_ADDR+4, vocm);
    } else if (id == SR_CONF_PROBE_VGAIN) {
        const uint64_t vgain = g_variant_get_uint64(data) << 8;
        int i;
        for (i = 0; ch->vga_ptr && (ch->vga_ptr+i)->id; i++) {
            if ((ch->vga_ptr+i)->key == ch->vdiv) {
                (ch->vga_ptr+i)->vgain = vgain;
            }
        }
        ret = dsl_wr_dso(sdi, dso_cmd_gen(sdi, ch, SR_CONF_PROBE_VDIV));
        if (ret == SR_OK)
            sr_dbg("%s: setting VDIV of channel %d to %d mv",
                __func__, ch->index, ch->vdiv);
        else
            sr_dbg("%s: setting VDIV of channel %d to %d mv failed",
                __func__, ch->index, ch->vdiv);
    } else if (id == SR_CONF_PROBE_PREOFF) {
        uint16_t preoff = g_variant_get_uint16(data);
        if (devc->profile->dev_caps.feature_caps & CAPS_FEATURE_PREOFF) {
            double voltage_off = (2.0 * preoff / devc->profile->dev_caps.default_pwmmargin  - 1) * ch->vdiv;
            double trans_coarse = (ch->vdiv < 500) ? (ch->vpos_trans >> 8)/DSCOPE_TRANS_CMULTI : (ch->vpos_trans >> 8);
            double trans_fine = (ch->vdiv < 500) ? (ch->vpos_trans & 0x00ff) / 1000.0 : (ch->vpos_trans & 0x00ff) / DSCOPE_TRANS_FMULTI;

            uint16_t default_preoff = get_default_preoff(sdi, ch);
            int preoff_coarse = floor(voltage_off / trans_coarse + 0.5);
            int preoff_fine = floor(-(voltage_off - preoff_coarse*trans_coarse)/trans_fine + 0.5);
            preoff_coarse = (default_preoff >> 10) + preoff_coarse;
            preoff_fine = (default_preoff&0x03ff) + preoff_fine;
            preoff = (preoff_coarse << 10) + preoff_fine;
        }
        int i;
        for (i = 0; ch->vga_ptr && (ch->vga_ptr+i)->id; i++) {
            if ((ch->vga_ptr+i)->key == ch->vdiv) {
                (ch->vga_ptr+i)->preoff = preoff;
            }
        }
        if (devc->status != DSL_FINISH)
            ret = dsl_wr_dso(sdi, dso_cmd_gen(sdi, ch, SR_CONF_PROBE_OFFSET));
        else
            ret = SR_OK;

        if (ret == SR_OK)
            sr_dbg("%s: setting OFFSET of channel %d to %d",
                __func__, ch->index, ch->offset);
        else
            sr_dbg("%s: setting OFFSET of channel %d to %d failed",
                __func__, ch->index, ch->offset);
    } else if (id == SR_CONF_PROBE_COMB_COMP) {
        ch->comb_comp = g_variant_get_int16(data);
        ret = dsl_wr_dso(sdi, dso_cmd_gen(sdi, ch, SR_CONF_PROBE_VDIV));
        if (ret == SR_OK)
            sr_dbg("%s: setting COMB_COMP of channel %d to %d mv",
                __func__, ch->index, ch->comb_comp);
        else
            sr_dbg("%s: setting COMB_COMP of channel %d to %d mv failed",
                __func__, ch->index, ch->comb_comp);
    } else {
        ret = SR_ERR_NA;
	}

	return ret;
}

static int config_list(int key, GVariant **data, const struct sr_dev_inst *sdi,
                       const struct sr_channel_group *cg)
{
    struct DSL_context *devc;

    (void)cg;
    devc = sdi->priv;

    if (dsl_config_list(key, data, sdi, cg) == SR_OK)
        return SR_OK;

    switch (key) {
    case SR_CONF_DEVICE_OPTIONS:
//		*data = g_variant_new_fixed_array(G_VARIANT_TYPE_INT32,
//				hwcaps, ARRAY_SIZE(hwcaps), sizeof(int32_t));
        *data = g_variant_new_from_data(G_VARIANT_TYPE("ai"),
                hwoptions, ARRAY_SIZE(hwoptions)*sizeof(int32_t), TRUE, NULL, NULL);
        break;
    case SR_CONF_DEVICE_SESSIONS:
        if (sdi->mode == DSO)
            *data = g_variant_new_from_data(G_VARIANT_TYPE("ai"),
                    sessions_dso, ARRAY_SIZE(sessions_dso)*sizeof(int32_t), TRUE, NULL, NULL);
        else if (sdi->mode == ANALOG)
            *data = g_variant_new_from_data(G_VARIANT_TYPE("ai"),
                    sessions_daq, ARRAY_SIZE(sessions_daq)*sizeof(int32_t), TRUE, NULL, NULL);
        break;
    case SR_CONF_OPERATION_MODE:
        *data = g_variant_new_strv(get_opmodes(devc), ARRAY_SIZE(opmodes));
        break;
    case SR_CONF_BANDWIDTH_LIMIT:
        *data = g_variant_new_strv(get_bandwidths(devc), ARRAY_SIZE(bandwidths));
        break;
    default:
        return SR_ERR_NA;
    }

    return SR_OK;
}

static int dso_zero(const struct sr_dev_inst *sdi)
{
    struct DSL_context *devc = sdi->priv;
    GSList *l;
    int ret;
    struct sr_usb_dev_inst *usb;
    struct libusb_device_handle *hdl;
    struct ctl_wr_cmd wr_cmd;

    static uint64_t vdiv_back[2];
    struct sr_channel *probe0 = NULL, *probe1 = NULL;
    const uint16_t offset_top = 20;
    const uint16_t offset_bom = ((1 << channel_modes[devc->ch_mode].unit_bits) - 1) - offset_top;
    const uint16_t offset_mid = (1 << (channel_modes[devc->ch_mode].unit_bits - 1));
    const uint16_t max_trans = ((1 << 10) - 1);
    const uint8_t value_min = 0;
    const uint8_t value_max = (1 << channel_modes[devc->ch_mode].unit_bits) - 1;

    const int zero_interval = 10;
    const double margin_pass = 0.3;
    int end_cnt = 0;
    static gboolean trans_fix_done = FALSE;
    static gboolean mid_zero_done = FALSE;
    static double margin[2];
    //static double offset[2];

    usb = sdi->conn;
    hdl = usb->devhdl;
    for(l = sdi->channels; l; l = l->next) {
        struct sr_channel *probe = (struct sr_channel *)l->data;
        if (probe->index == 0)
            probe0 = probe;
        if (probe->index == 1)
            probe1 = probe;
        vdiv_back[probe->index] = probe->vdiv;
    }

    if (!trans_fix_done && devc->zero_stage == 0) {
        ret = SR_OK;
        if (!(devc->profile->dev_caps.feature_caps & CAPS_FEATURE_PREOFF)) {
            if (devc->zero_pcnt == 0*zero_interval) {
                for(l = sdi->channels; l; l = l->next) {
                    struct sr_channel *probe = (struct sr_channel *)l->data;
                    probe->zero_offset = offset_bom;
                    ret = dsl_wr_dso(sdi, dso_cmd_gen(sdi, probe, SR_CONF_PROBE_OFFSET));
                }
            }
            if (devc->zero_pcnt == 1*zero_interval) {
                margin[0] = (devc->mstatus.ch0_acc_mean * 1.0 / devc->limit_samples);
                margin[1] = (devc->mstatus.ch1_acc_mean * 1.0 / devc->limit_samples);
                if (margin[0] >= value_max || margin[1] >= value_max)
                    ret = SR_ERR;
            }
            if (devc->zero_pcnt == 1*zero_interval+1) {
                for(l = sdi->channels; l; l = l->next) {
                    struct sr_channel *probe = (struct sr_channel *)l->data;
                    probe->zero_offset = offset_top;
                    ret = dsl_wr_dso(sdi, dso_cmd_gen(sdi, probe, SR_CONF_PROBE_OFFSET));
                }
            }
            if (devc->zero_pcnt == 2*zero_interval) {
                double top0 = (devc->mstatus.ch0_acc_mean * 1.0 / devc->limit_samples);
                double top1 = (devc->mstatus.ch1_acc_mean * 1.0 / devc->limit_samples);
                if (top0 <= value_min || top1 <= value_min) {
                    ret = SR_ERR;
                } else {
                    margin[0] -= top0;
                    margin[1] -= top1;
                    for(l = sdi->channels; l; l = l->next) {
                        struct sr_channel *probe = (struct sr_channel *)l->data;
                        margin[probe->index] -= (offset_bom - offset_top);
                        if (fabs(margin[probe->index]) > margin_pass) {
                            margin[probe->index] = margin[probe->index] > 0 ? ceil(margin[probe->index]) : floor(margin[probe->index]);
                            probe->vpos_trans = min(probe->vpos_trans - margin[probe->index], max_trans);
                            ret = dsl_wr_dso(sdi, dso_cmd_gen(sdi, probe0, SR_CONF_PROBE_OFFSET));
                        } else {
                            margin[probe->index] = 0;
                        }
                    }
                    trans_fix_done = (margin[0] == 0) && (margin[1] == 0);
                    devc->zero_pcnt = trans_fix_done ? 0*zero_interval : 0*zero_interval-1;
                }
            }
        } else {
            trans_fix_done = TRUE;
        }

        if (!trans_fix_done && ret == SR_OK)
            devc->zero_pcnt++;
    } else if (!mid_zero_done) {
        if (devc->zero_pcnt == 0) {
            for(l = sdi->channels; l; l = l->next) {
                struct sr_channel *probe = (struct sr_channel *)l->data;
                probe->vdiv = (probe->vga_ptr+devc->zero_stage)->key;
                if (probe->vdiv == 0) {
                    probe->vdiv = vdiv_back[probe->index];
                    mid_zero_done = TRUE;
                    break;
                }
                ret = dsl_wr_dso(sdi, dso_cmd_gen(sdi, probe, SR_CONF_PROBE_VDIV));
                probe->zero_offset = offset_mid;
                ret = dsl_wr_dso(sdi, dso_cmd_gen(sdi, probe, SR_CONF_PROBE_OFFSET));
                // must after offset setting
                probe->vdiv = vdiv_back[probe->index];
            }
        }

        if (devc->zero_pcnt == zero_interval) {
            margin[0] = offset_mid - (devc->mstatus.ch0_acc_mean * 1.0 / devc->limit_samples);
            margin[1] = offset_mid - (devc->mstatus.ch1_acc_mean * 1.0 / devc->limit_samples);
            if (fabs(margin[0]) < margin_pass && fabs(margin[1]) < margin_pass) {
                devc->zero_stage++;
            } else {
                if (devc->profile->dev_caps.feature_caps & CAPS_FEATURE_PREOFF) {
                    for(l = sdi->channels; l; l = l->next) {
                        struct sr_channel *probe = (struct sr_channel *)l->data;
                        double trans_coarse = ((probe->vga_ptr+devc->zero_stage)->key < 500) ? (probe->vpos_trans >> 8)/DSCOPE_TRANS_CMULTI : (probe->vpos_trans >> 8);
                        double trans_fine = ((probe->vga_ptr+devc->zero_stage)->key < 500) ? (probe->vpos_trans & 0x00ff) / 1000.0 : (probe->vpos_trans & 0x00ff) / DSCOPE_TRANS_FMULTI;

                        double voltage_margin = margin[probe->index] * (probe->vga_ptr+devc->zero_stage)->key * 10 / 255.0;
                        uint16_t last_preoff = (probe->vga_ptr+devc->zero_stage)->preoff;
                        int preoff_coarse = floor(voltage_margin / trans_coarse + 0.5);
                        int preoff_fine = floor(-(voltage_margin - preoff_coarse*trans_coarse)/trans_fine + 0.5);
                        preoff_coarse = (last_preoff >> 10) + preoff_coarse;
                        preoff_fine = (last_preoff&0x03ff) + preoff_fine;
                        (probe->vga_ptr+devc->zero_stage)->preoff = (preoff_coarse << 10) + preoff_fine;
                    }
                } else {
                    for(l = sdi->channels; l; l = l->next) {
                        struct sr_channel *probe = (struct sr_channel *)l->data;
                        (probe->vga_ptr+devc->zero_stage)->preoff += margin[probe->index] > 0 ? ceil(margin[probe->index]) : floor(margin[probe->index]);
                    }
                }
            }
            devc->zero_pcnt = 0;
        } else if (!mid_zero_done) {
            devc->zero_pcnt++;
        }
    } else {
        ret = SR_OK;
        end_cnt = 0*zero_interval + 1;
        if (devc->profile->dev_caps.feature_caps & CAPS_FEATURE_HMCAD1511) {
//            if (devc->zero_pcnt == 0*zero_interval+1) {
//                for(l = sdi->channels; l; l = l->next) {
//                    struct sr_channel *probe = (struct sr_channel *)l->data;
//                    probe->zero_offset = offset_bom;
//                    ret = dsl_wr_dso(sdi, dso_cmd_gen(sdi, probe, SR_CONF_PROBE_OFFSET));
//                }
//            }
//            if (devc->zero_pcnt == 1*zero_interval) {
//                offset[0] = (devc->mstatus.ch0_acc_mean * 1.0 / devc->limit_samples);
//                offset[1] = (devc->mstatus.ch1_acc_mean * 1.0 / devc->limit_samples);
//                if (margin[0] >= value_max || margin[1] >= value_max)
//                    ret = SR_ERR;
//            }
//            if (devc->zero_pcnt == 1*zero_interval+1) {
//                dsl_config_adc(sdi, adc_single_ch0);
//                probe0->enabled = TRUE;
//                probe1->enabled = FALSE;

//                probe0->zero_offset = offset_bom;
//                ret = dsl_wr_dso(sdi, dso_cmd_gen(sdi, probe0, SR_CONF_PROBE_OFFSET));
//            }
//            if (devc->zero_pcnt == 2*zero_interval) {
//                margin[0] = offset[0] - (devc->mstatus.ch0_acc_mean * 1.0 / devc->limit_samples);
//                if (fabs(margin[0]) > margin_pass) {
//                    probe0->comb_comp -= margin[0] > 0 ? ceil(margin[0]) : floor(margin[0]);
//                    ret = dsl_wr_dso(sdi, dso_cmd_gen(sdi, probe0, SR_CONF_PROBE_OFFSET));
//                    devc->zero_pcnt = 1*zero_interval+1;
//                }
//            }
//            if (devc->zero_pcnt == 2*zero_interval+1) {
//                dsl_config_adc(sdi, adc_single_ch3);
//                probe0->enabled = FALSE;
//                probe1->enabled = TRUE;

//                probe1->zero_offset = offset_bom;
//                ret = dsl_wr_dso(sdi, dso_cmd_gen(sdi, probe1, SR_CONF_PROBE_OFFSET));
//            }
//            if (devc->zero_pcnt == 3*zero_interval) {
//                margin[1] = offset[1] - (devc->mstatus.ch1_acc_mean * 1.0 / devc->limit_samples);
//                if (fabs(margin[1]) > margin_pass) {
//                    probe1->comb_comp -= margin[1] > 0 ? ceil(margin[1]) : floor(margin[1]);
//                    ret = dsl_wr_dso(sdi, dso_cmd_gen(sdi, probe1, SR_CONF_PROBE_OFFSET));
//                    devc->zero_pcnt = 2*zero_interval+1;
//                }
//            }
//            end_cnt = 3*zero_interval + 1;
        } else {
            if (devc->zero_pcnt == 0*zero_interval+1) {
                ret = dsl_wr_reg(sdi, COMB_ADDR+6, 0b1101);
                wr_cmd.header.dest = DSL_CTL_DSO_EN1;
                wr_cmd.data[0] = (uint8_t)~bmCH_CH1;
                wr_cmd.header.size = 1;
                ret = command_ctl_wr(hdl, wr_cmd);

                probe0->zero_offset = offset_top;
                ret = dsl_wr_dso(sdi, dso_cmd_gen(sdi, probe0, SR_CONF_PROBE_OFFSET));
            } else if (devc->zero_pcnt == 1*zero_interval) {
                probe0->comb_diff_top = ((devc->mstatus.ch0_acc_mean * 2.0 - devc->mstatus.ch1_acc_mean * 2.0) / devc->limit_samples);
                probe0->zero_offset = offset_bom;
                ret = dsl_wr_dso(sdi, dso_cmd_gen(sdi, probe0, SR_CONF_PROBE_OFFSET));
            } else if (devc->zero_pcnt == 2*zero_interval) {
                probe0->comb_diff_bom = ((devc->mstatus.ch0_acc_mean * 2.0 - devc->mstatus.ch1_acc_mean * 2.0) / devc->limit_samples);
            }

            if (devc->zero_pcnt == 2*zero_interval+1) {
                ret = dsl_wr_reg(sdi, COMB_ADDR+6, 0b1110);
                wr_cmd.header.dest = DSL_CTL_DSO_EN1;
                wr_cmd.data[0] = bmCH_CH1;
                wr_cmd.header.size = 1;
                ret = command_ctl_wr(hdl, wr_cmd);
                wr_cmd.header.dest = DSL_CTL_DSO_EN0;
                wr_cmd.data[0] = (uint8_t)~bmCH_CH0;
                wr_cmd.header.size = 1;
                ret = command_ctl_wr(hdl, wr_cmd);

                probe1->zero_offset = offset_top;
                ret = dsl_wr_dso(sdi, dso_cmd_gen(sdi, probe1, SR_CONF_PROBE_OFFSET));
            } else if (devc->zero_pcnt == 3*zero_interval) {
                probe1->comb_diff_top = ((devc->mstatus.ch1_acc_mean * 2.0 - devc->mstatus.ch0_acc_mean * 2.0) / devc->limit_samples);
                probe1->zero_offset = offset_bom;
                ret = dsl_wr_dso(sdi, dso_cmd_gen(sdi, probe1, SR_CONF_PROBE_OFFSET));
            } else if (devc->zero_pcnt == 4*zero_interval) {
                probe1->comb_diff_bom = ((devc->mstatus.ch1_acc_mean * 2.0 - devc->mstatus.ch0_acc_mean * 2.0) / devc->limit_samples);
            }

            end_cnt = 4*zero_interval+1;
        }

        if (ret == SR_OK)
            devc->zero_pcnt++;

        if (devc->zero_pcnt == end_cnt) {
            for(l = sdi->channels; l; l = l->next) {
                struct sr_channel *probe = (struct sr_channel *)l->data;
                probe->vdiv = vdiv_back[probe->index];
//                if (!(devc->profile->dev_caps.feature_caps & CAPS_FEATURE_HMCAD1511))
//                    probe->comb_comp = ((1 << channel_modes[devc->ch_mode].unit_bits) - 1) * 0.7 * 2 /
//                                       min(abs(probe->comb_diff_top), abs(probe->comb_diff_bom));

            }

            ret = dsl_wr_reg(sdi, COMB_ADDR+6, 0b0011);
            wr_cmd.header.dest = DSL_CTL_DSO_EN0;
            wr_cmd.data[0] = bmCH_CH0;
            wr_cmd.header.size = 1;
            ret = command_ctl_wr(hdl, wr_cmd);
            wr_cmd.header.dest = DSL_CTL_DSO_EN1;
            wr_cmd.data[0] = bmCH_CH1;
            wr_cmd.header.size = 1;
            ret = command_ctl_wr(hdl, wr_cmd);

            devc->zero = FALSE;
            trans_fix_done = FALSE;
            mid_zero_done = FALSE;
            dso_init(sdi);
        }
    }

    return ret;
}

static int dso_tune(const struct sr_dev_inst *sdi)
{
    struct DSL_context *devc = sdi->priv;
    int ret = SR_OK;
    double margin;
    static uint64_t vdiv_back = 0;
    static uint16_t offset_back = 0;
    static int coupling_back = SR_DC_COUPLING;
    const uint8_t mux0[8] = {0x09, 0x0f, 0x0b, 0x0d, 0x07, 0x05, 0x01, 0x03};
    const uint8_t mux1[8] = {0x09, 0x0f, 0x0b, 0x0d, 0x0e, 0x0c, 0x08, 0x0a};
    const uint8_t *mux = (devc->profile->dev_caps.feature_caps & CAPS_FEATURE_POGOPIN) ? mux1 : mux0;

    if (devc->tune_probe && devc->tune_stage == -1) {
        vdiv_back = devc->tune_probe->vdiv;
        offset_back = devc->tune_probe->offset;
        coupling_back = devc->tune_probe->coupling;

        devc->tune_stage = 0;
        ret = dsl_wr_ext(sdi, 0x03, 0x00);
        ret = dsl_wr_ext(sdi, 0x01, mux[devc->tune_stage]);
        devc->tune_probe->vdiv = (devc->tune_probe->vga_ptr + devc->tune_stage)->key;
        devc->tune_probe->offset = (1 << (channel_modes[devc->ch_mode].unit_bits - 1));
        devc->tune_probe->coupling = SR_AC_COUPLING;
        ret = dsl_wr_dso(sdi, dso_cmd_gen(sdi, devc->tune_probe, SR_CONF_PROBE_VDIV));
        ret = dsl_wr_dso(sdi, dso_cmd_gen(sdi, devc->tune_probe, SR_CONF_PROBE_OFFSET));
        ret = dsl_wr_dso(sdi, dso_cmd_gen(sdi, devc->tune_probe, SR_CONF_PROBE_COUPLING));
    } else if (devc->tune_probe && devc->profile->dev_caps.vdivs[devc->tune_stage] != 0) {
        if (devc->tune_pcnt == 10) {
            devc->tune_pcnt = 0;
            margin = (devc->tune_probe->coupling == SR_AC_COUPLING) ? 127.5 : 25.5;
            if (devc->tune_probe->index == 0)
                margin -= (devc->mstatus.ch0_acc_mean * 1.0 / devc->limit_samples);
            else
                margin -= (devc->mstatus.ch1_acc_mean * 1.0 / devc->limit_samples);

            if ((devc->tune_probe->coupling == SR_AC_COUPLING) && (abs(margin) < 0.5)) {
                devc->tune_probe->coupling = SR_DC_COUPLING;
                ret = dsl_wr_dso(sdi, dso_cmd_gen(sdi, devc->tune_probe, SR_CONF_PROBE_COUPLING));
            } else if (devc->tune_probe->coupling == SR_AC_COUPLING){
                (devc->tune_probe->vga_ptr+devc->tune_stage)->preoff += margin;
                ret = dsl_wr_dso(sdi, dso_cmd_gen(sdi, devc->tune_probe, SR_CONF_PROBE_OFFSET));
            } else if ((devc->tune_probe->coupling == SR_DC_COUPLING) && (abs(margin) < 0.5)) {
                devc->tune_stage++;
                if (devc->profile->dev_caps.vdivs[devc->tune_stage] != 0) {
                    ret = dsl_wr_ext(sdi, 0x01, mux[devc->tune_stage]);
                    devc->tune_probe->vdiv = (devc->tune_probe->vga_ptr + devc->tune_stage)->key;
                    devc->tune_probe->coupling = SR_AC_COUPLING;
                    ret = dsl_wr_dso(sdi, dso_cmd_gen(sdi, devc->tune_probe, SR_CONF_PROBE_VDIV));
                    ret = dsl_wr_dso(sdi, dso_cmd_gen(sdi, devc->tune_probe, SR_CONF_PROBE_OFFSET));
                    ret = dsl_wr_dso(sdi, dso_cmd_gen(sdi, devc->tune_probe, SR_CONF_PROBE_COUPLING));
                }else {
                    ret = dsl_wr_ext(sdi, 0x01, mux[0]);
                    devc->tune_probe->vdiv = vdiv_back;
                    devc->tune_probe->offset = offset_back;
                    devc->tune_probe->coupling = coupling_back;
                    ret = dsl_wr_dso(sdi, dso_cmd_gen(sdi, devc->tune_probe, SR_CONF_PROBE_VDIV));
                    ret = dsl_wr_dso(sdi, dso_cmd_gen(sdi, devc->tune_probe, SR_CONF_PROBE_OFFSET));
                    ret = dsl_wr_dso(sdi, dso_cmd_gen(sdi, devc->tune_probe, SR_CONF_PROBE_COUPLING));
                    devc->tune = FALSE;
                }
            } else if (devc->tune_probe->coupling == SR_DC_COUPLING){
                (devc->tune_probe->vga_ptr + devc->tune_stage)->vgain -= ceil(margin*1024);
                ret = dsl_wr_dso(sdi, dso_cmd_gen(sdi, devc->tune_probe, SR_CONF_PROBE_VDIV));
            }
        }
        if (ret == SR_OK)
            devc->tune_pcnt++;
    }

    return ret;
}

static int dev_open(struct sr_dev_inst *sdi)
{
    gboolean fpga_done;
    int ret;
    GSList *l;
    gboolean zeroed;
    struct DSL_context *devc = sdi->priv;

    if ((ret = dsl_dev_open(di, sdi, &fpga_done)) == SR_OK) {
        // load zero informations
        for(l = sdi->channels; l; l = l->next) {
            struct sr_channel *probe = (struct sr_channel *)l->data;
            zeroed = dso_load_eep(sdi, probe, fpga_done);
            if (!zeroed)
                break;
        }
        if (!zeroed) {
            config_set(SR_CONF_ZERO, g_variant_new_boolean(TRUE), sdi, NULL, NULL);
            sr_info("Zero have not been setted!");
        }
        if (!fpga_done) {
            if (devc->profile->dev_caps.feature_caps & CAPS_FEATURE_HMCAD1511) {
                dsl_config_adc(sdi, adc_init_fix);
                dsl_config_adc(sdi, adc_clk_init_1g);
                dsl_config_adc(sdi, adc_power_down);
            }
            dso_init(sdi);
        }
    }

    return ret;
}

static int dev_close(struct sr_dev_inst *sdi)
{
    int ret;
    ret = dsl_dev_close(sdi);
    return ret;
}

static int cleanup(void)
{
	int ret;
	struct drv_context *drvc;

	if (!(drvc = di->priv))
        return SR_OK;

	ret = dev_clear();

	g_free(drvc);
	di->priv = NULL;

	return ret;
}

static void remove_sources(struct DSL_context *devc)
{
    int i;
    sr_info("%s: remove fds from polling", __func__);
    /* Remove fds from polling. */
    for (i = 0; devc->usbfd[i] != -1; i++)
        sr_source_remove(devc->usbfd[i]);
    g_free(devc->usbfd);
}

static int receive_data(int fd, int revents, const struct sr_dev_inst *sdi)
{
    int completed = 0;
    struct timeval tv;
    struct drv_context *drvc;
    struct DSL_context *devc;

    (void)fd;
    (void)revents;

    drvc = di->priv;
    devc = sdi->priv;

    tv.tv_sec = tv.tv_usec = 0;
    libusb_handle_events_timeout_completed(drvc->sr_ctx->libusb_ctx, &tv, &completed);

    if (devc->zero && devc->trf_completed) {
        dso_zero(sdi);
    }
    if (devc->tune && devc->trf_completed) {
        dso_tune(sdi);
    }

    if (devc->status == DSL_FINISH) {
        remove_sources(devc);
    }

    devc->trf_completed = 0;
    return TRUE;
}

static int dev_acquisition_start(struct sr_dev_inst *sdi, void *cb_data)
{
    (void)cb_data;

    struct DSL_context *devc;
    struct sr_usb_dev_inst *usb;
    struct drv_context *drvc;
    const struct libusb_pollfd **lupfd;
	unsigned int i;
    int ret;
    struct ctl_wr_cmd wr_cmd;
    GSList *l;

    if (sdi->status != SR_ST_ACTIVE)
        return SR_ERR_DEV_CLOSED;

    drvc = di->priv;
    devc = sdi->priv;
    usb = sdi->conn;

    //devc->cb_data = cb_data;
    devc->cb_data = sdi;
    devc->num_samples = 0;
    devc->empty_transfer_count = 0;
    devc->status = DSL_INIT;
    devc->num_transfers = 0;
    devc->submitted_transfers = 0;
    devc->actual_samples = (devc->limit_samples + 1023ULL) & ~1023ULL;
	devc->abort = FALSE;
    devc->mstatus_valid = FALSE;
    devc->overflow = FALSE;
    devc->instant_tail_bytes = dsl_header_size(devc);

	/* Configures devc->trigger_* and devc->sample_wide */
    if (dsl_configure_probes(sdi) != SR_OK) {
        sr_err("%s: Failed to configure probes.", __func__);
        return SR_ERR;
	}

    /* Stop Previous GPIF acquisition */
    wr_cmd.header.dest = DSL_CTL_STOP;
    wr_cmd.header.size = 0;
    if ((ret = command_ctl_wr(usb->devhdl, wr_cmd)) != SR_OK) {
        sr_err("%s: Stop DSCope acquisition failed!", __func__);
        return ret;
    } else {
        sr_info("%s: Stop Previous DSCope acquisition!", __func__);
    }

    /* Arm FPGA before acquisition start*/
    if (devc->profile->dev_caps.feature_caps & CAPS_FEATURE_HMCAD1511) {
        if (dsl_en_ch_num(sdi) == 2) {
            dsl_config_adc(sdi, adc_dual_ch03);
        } else if (dsl_en_ch_num(sdi) == 1) {
            for (l = sdi->channels; l; l = l->next) {
                struct sr_channel *probe = (struct sr_channel *)l->data;
                if (probe->enabled && probe->index == 0) {
                    dsl_config_adc(sdi, adc_single_ch0);
                    break;
                } else if (probe->enabled && probe->index == 1) {
                    dsl_config_adc(sdi, adc_single_ch3);
                    break;
                }
            }
        }
        //dsl_config_adc(sdi, adc_power_up);
    }
    if ((ret = dsl_fpga_arm(sdi)) != SR_OK) {
        sr_err("%s: Arm FPGA failed!", __func__);
        return ret;
    }

    if (devc->zero && devc->zero_stage == -1) {
        // initialize before Auto Calibration
        if ((ret = dso_init(sdi)) != SR_OK) {
            sr_err("%s: DSO zero initialization failed!", __func__);
            return ret;
        }
        devc->zero_stage = 0;
    }

    /*
     * settings must be updated before acquisition
     */
    if (sdi->mode == DSO) {
        devc->trigger_hpos =  devc->trigger_hrate * dsl_en_ch_num(sdi) * devc->limit_samples / 200.0;
        ret = dsl_wr_dso(sdi, dso_cmd_gen(sdi, NULL, SR_CONF_HORIZ_TRIGGERPOS));
        if (ret != SR_OK)
            sr_dbg("%s: setting DSO Horiz Trigger Position to %d failed", __func__, devc->trigger_hpos);

        for(l = sdi->channels; l; l = l->next) {
            struct sr_channel *probe = (struct sr_channel *)l->data;
            ret = dsl_wr_dso(sdi, dso_cmd_gen(sdi, probe, SR_CONF_PROBE_VDIV));
            if (ret != SR_OK)
                sr_err("%s: Set VDIV of channel %d command failed!", __func__, probe->index);
            ret = dsl_wr_dso(sdi, dso_cmd_gen(sdi, probe, SR_CONF_PROBE_OFFSET));
            if (ret != SR_OK)
                sr_err("%s: Set OFFSET of channel %d command failed!", __func__, probe->index);
        }
    }

    /* setup and submit usb transfer */
    if ((ret = dsl_start_transfers(devc->cb_data)) != SR_OK) {
        sr_err("%s: Could not submit usb transfer"
               "(%d)%d", __func__, ret, errno);
        return ret;
    }

    /* setup callback function for data transfer */
    lupfd = libusb_get_pollfds(drvc->sr_ctx->libusb_ctx);
    for (i = 0; lupfd[i]; i++);
    if (!(devc->usbfd = g_try_malloc(sizeof(struct libusb_pollfd) * (i + 1))))
    	return SR_ERR;
    for (i = 0; lupfd[i]; i++) {
        sr_source_add(lupfd[i]->fd, lupfd[i]->events,
                  dsl_get_timeout(sdi), receive_data, sdi);
        devc->usbfd[i] = lupfd[i]->fd;
    }
    devc->usbfd[i] = -1;
    free(lupfd);

    wr_cmd.header.dest = DSL_CTL_START;
    wr_cmd.header.size = 0;
    if ((ret = command_ctl_wr(usb->devhdl, wr_cmd)) != SR_OK) {
        devc->status = DSL_ERROR;
        devc->abort = TRUE;
        return ret;
    }
    devc->status = DSL_START;

	/* Send header packet to the session bus. */
    //std_session_send_df_header(cb_data, LOG_PREFIX);
    std_session_send_df_header(sdi, LOG_PREFIX);

    return SR_OK;
}

static int dev_acquisition_stop(const struct sr_dev_inst *sdi, void *cb_data)
{
    struct DSL_context *devc = sdi->priv;
    int ret = dsl_dev_acquisition_stop(sdi, cb_data);
    if (devc->profile->dev_caps.feature_caps & CAPS_FEATURE_HMCAD1511) {
        dsl_config_adc(sdi, adc_power_down);
    }
    return ret;
}

static int dev_status_get(const struct sr_dev_inst *sdi, struct sr_status *status, gboolean prg, int begin, int end)
{
    int ret = dsl_dev_status_get(sdi, status, prg, begin, end);
    return ret;
}

SR_PRIV struct sr_dev_driver DSCope_driver_info = {
    .name = "DSCope",
    .longname = "DSCope (generic driver for DScope oscilloscope)",
	.api_version = 1,
	.init = init,
	.cleanup = cleanup,
	.scan = scan,
	.dev_list = dev_list,
    .dev_mode_list = dev_mode_list,
	.dev_clear = dev_clear,
	.config_get = config_get,
	.config_set = config_set,
	.config_list = config_list,
	.dev_open = dev_open,
	.dev_close = dev_close,
    .dev_status_get = dev_status_get,
	.dev_acquisition_start = dev_acquisition_start,
	.dev_acquisition_stop = dev_acquisition_stop,
	.priv = NULL,
};
